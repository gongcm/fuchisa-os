// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

///! Implements a DHCPv6 client.
use {
    anyhow::{Context as _, Result},
    dhcpv6::{
        client::{Action, Actions, Dhcpv6ClientStateMachine, Dhcpv6ClientTimerType},
        protocol::{Dhcpv6Message, Dhcpv6OptionCode, ProtocolError},
    },
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_net as fnet,
    fidl_fuchsia_net_dhcpv6::{
        ClientMarker, ClientRequest, ClientRequestStream, ClientWatchServersResponder,
        NewClientParams, OperationalModels, RequestableOptionCode, Stateless, DEFAULT_CLIENT_PORT,
    },
    fidl_fuchsia_net_ext as fnetext, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::{
        future::{AbortHandle, Abortable},
        select, stream,
        stream::futures_unordered::FuturesUnordered,
        task, Future, FutureExt as _, StreamExt as _, TryStreamExt as _,
    },
    net_declare::std_ip,
    packet::ParsablePacket,
    rand::{rngs::StdRng, thread_rng, FromEntropy, Rng},
    std::{
        collections::{hash_map::Entry, HashMap},
        convert::TryFrom,
        net::{IpAddr, Ipv6Addr, SocketAddr},
        num::TryFromIntError,
        pin::Pin,
        time::Duration,
    },
};

#[derive(Debug, thiserror::Error)]
pub enum ClientError {
    #[error("no timer scheduled for {:?}", _0)]
    MissingTimer(Dhcpv6ClientTimerType),
    #[error("a timer is already scheduled for {:?}", _0)]
    TimerAlreadyExist(Dhcpv6ClientTimerType),
    #[error("got overflow while applying timeout {:?}", _0)]
    TimerOverflow(Duration),
    #[error("IO error: {}", _0)]
    Io(std::io::Error),
    #[error("protocol error: {}", _0)]
    Protocol(ProtocolError),
    #[error("fidl error: {}", _0)]
    Fidl(fidl::Error),
    #[error("got watch request while the previous one is pending")]
    DoubleWatch(),
    #[error("unsupported DHCPv6 operational models: {:?}, only stateless is supported", _0)]
    UnsupportedModels(OperationalModels),
}

/// The port DHCPv6 servers and relay agents should listen on according to [RFC 8415, Section 7.2].
///
/// [RFC 8415, Section 7.2]: https://tools.ietf.org/html/rfc8415#section-7.2
const DHCPV6_AGENT_AND_SERVER_PORT: u16 = 547;

/// A link-scoped multicast address used by a client to communicate with neighboring relay agents
/// and servers according to [RFC 8415, Section 7.1].
///
/// [RFC 8415, Section 7.1]: https://tools.ietf.org/html/rfc8415#section-7.1
const ALL_DHCP_AGENTS_AND_SERVERS: IpAddr = std_ip!(ff02::1:2);

/// Theoretical size limit for UDP datagrams.
///
/// NOTE: This does not take [jumbograms](https://tools.ietf.org/html/rfc2675) into account.
const MAX_UDP_DATAGRAM_SIZE: usize = 65_535;

/// A future that waits on a timer and always resolves to a `Dhcpv6ClientTimerType`.
// TODO(http://fxbug.dev/53611): pull TimerFut to a common place, there's an templated version of
// this struct in netstack3.
#[derive(Debug)]
#[must_use = "futures do nothing unless you `.await` or poll them"]
struct TimerFut {
    timer_type: Option<Dhcpv6ClientTimerType>,
    timer: fasync::Timer,
}

impl TimerFut {
    pin_utils::unsafe_pinned!(timer: fasync::Timer);

    fn new(timer_type: Dhcpv6ClientTimerType, time: fasync::Time) -> Self {
        Self { timer_type: Some(timer_type), timer: fasync::Timer::new(time) }
    }
}

impl Future for TimerFut {
    type Output = Dhcpv6ClientTimerType;

    fn poll(mut self: Pin<&mut Self>, cx: &mut task::Context<'_>) -> task::Poll<Self::Output> {
        match self.as_mut().timer().poll(cx) {
            task::Poll::Ready(()) => task::Poll::Ready(
                self.timer_type
                    .take()
                    .expect("TimerFut must not be polled after it returned `Poll::Ready`"),
            ),
            task::Poll::Pending => task::Poll::Pending,
        }
    }
}

/// A DHCPv6 client.
pub(crate) struct Dhcpv6Client {
    /// Stores a responder to send DNS server updates.
    dns_responder: Option<ClientWatchServersResponder>,
    /// Maintains the state for the client.
    state_machine: Dhcpv6ClientStateMachine<StdRng>,
    /// The socket used to communicate with DHCPv6 servers.
    socket: fasync::net::UdpSocket,
    /// The address to send outgoing messages to.
    server_addr: SocketAddr,
    /// A collection of abort handles to all currently scheduled timers.
    timer_abort_handles: HashMap<Dhcpv6ClientTimerType, AbortHandle>,
    /// A set of all scheduled timers.
    timer_futs: FuturesUnordered<Abortable<TimerFut>>,
    /// A stream of FIDL requests to this client.
    request_stream: ClientRequestStream,
}

/// Converts a collection of `RequestableOptionCode` to `Dhcpv6OptionCode`.
fn to_dhcpv6_option_codes(codes: Vec<RequestableOptionCode>) -> Vec<Dhcpv6OptionCode> {
    codes
        .into_iter()
        .map(|option| match option {
            RequestableOptionCode::DnsServers => Dhcpv6OptionCode::DnsServers,
        })
        .collect()
}

/// Creates a state machine for the input operational models.
fn create_state_machine(
    transaction_id: [u8; 3],
    models: OperationalModels,
) -> Result<(Dhcpv6ClientStateMachine<StdRng>, Actions), ClientError> {
    match models {
        OperationalModels { stateless: Some(Stateless { options_to_request }) } => {
            Ok(Dhcpv6ClientStateMachine::start_information_request(
                transaction_id,
                options_to_request.map(to_dhcpv6_option_codes).unwrap_or(Vec::new()),
                StdRng::from_entropy(),
            ))
        }
        OperationalModels { stateless: None } => Err(ClientError::UnsupportedModels(models)),
    }
}

impl Dhcpv6Client {
    /// Starts the client in `models`.
    ///
    /// Input `transaction_id` is used to label outgoing messages and match incoming ones.
    pub(crate) async fn start(
        transaction_id: [u8; 3],
        models: OperationalModels,
        socket: fasync::net::UdpSocket,
        server_addr: SocketAddr,
        request_stream: ClientRequestStream,
    ) -> Result<Self, ClientError> {
        let (state_machine, actions) = create_state_machine(transaction_id, models)?;
        let mut client = Self {
            state_machine,
            socket,
            server_addr,
            request_stream,
            timer_abort_handles: HashMap::new(),
            timer_futs: FuturesUnordered::new(),
            dns_responder: None,
        };
        let () = client.run_actions(actions).await?;
        Ok(client)
    }

    /// Runs a list of actions sequentially.
    async fn run_actions(&mut self, actions: Actions) -> Result<(), ClientError> {
        stream::iter(actions)
            .map(Ok)
            .try_fold(self, |client, action| async move {
                match action {
                    Action::SendMessage(buf) => client.send_message(&buf).await?,
                    Action::ScheduleTimer(timer_type, timeout) => {
                        client.schedule_timer(timer_type, timeout)?
                    }
                    Action::CancelTimer(timer_type) => client.cancel_timer(timer_type)?,
                };
                Ok(client)
            })
            .await
            .map(|_: &mut Dhcpv6Client| ())
    }

    /// Multicasts a message to neighboring relay agents and servers.
    async fn send_message(&self, buf: &[u8]) -> Result<(), ClientError> {
        self.socket.send_to(buf, self.server_addr).await.map(|_: usize| ()).map_err(ClientError::Io)
    }

    /// Schedules a timer for `timer_type` to fire after `timeout`.
    fn schedule_timer(
        &mut self,
        timer_type: Dhcpv6ClientTimerType,
        timeout: Duration,
    ) -> Result<(), ClientError> {
        match self.timer_abort_handles.entry(timer_type) {
            Entry::Vacant(entry) => {
                let (handle, reg) = AbortHandle::new_pair();
                let _: &mut AbortHandle = entry.insert(handle);
                let () = self.timer_futs.push(Abortable::new(
                    TimerFut::new(
                        timer_type,
                        fasync::Time::after(zx::Duration::from_nanos(
                            i64::try_from(timeout.as_nanos()).map_err(|_: TryFromIntError| {
                                ClientError::TimerOverflow(timeout)
                            })?,
                        )),
                    ),
                    reg,
                ));
                Ok(())
            }
            Entry::Occupied(_) => Err(ClientError::TimerAlreadyExist(timer_type)),
        }
    }

    /// Cancels a previously scheduled timer for `timer_type`.
    fn cancel_timer(&mut self, timer_type: Dhcpv6ClientTimerType) -> Result<(), ClientError> {
        match self.timer_abort_handles.entry(timer_type) {
            Entry::Vacant(_) => Err(ClientError::MissingTimer(timer_type)),
            Entry::Occupied(entry) => Ok(entry.remove().abort()),
        }
    }

    /// Handles a timeout.
    async fn handle_timeout(
        &mut self,
        timer_type: Dhcpv6ClientTimerType,
    ) -> Result<(), ClientError> {
        let () = self.cancel_timer(timer_type)?; // This timer just fired.
        let actions = self.state_machine.handle_timeout(timer_type);
        self.run_actions(actions).await
    }

    /// Handles a received message.
    async fn handle_message_recv(&mut self, mut msg: &[u8]) -> Result<(), ClientError> {
        let msg = Dhcpv6Message::parse(&mut msg, ()).map_err(ClientError::Protocol)?;
        let actions = self.state_machine.handle_message_receive(msg);
        self.run_actions(actions).await
    }

    /// Handles a FIDL request sent to this client.
    fn handle_client_request(&mut self, request: ClientRequest) -> Result<(), ClientError> {
        match request {
            ClientRequest::WatchServers { responder } => match self.dns_responder {
                Some(_) => {
                    // Drop the previous responder to close the channel.
                    self.dns_responder = None;
                    Err(ClientError::DoubleWatch())
                }
                None => {
                    self.dns_responder = Some(responder);
                    Ok(())
                }
            },
        }
    }

    /// Handles the next event and returns the result.
    ///
    /// Takes a pre-allocated buffer to avoid repeated allocation.
    ///
    /// The returned `Option` is `None` if `request_stream` on the client is closed.
    async fn handle_next_event(&mut self, buf: &mut [u8]) -> Result<Option<()>, ClientError> {
        select! {
            timer_res = self.timer_futs.select_next_some() => {
                match timer_res {
                    Ok(timer_type) => {
                        let () = self.handle_timeout(timer_type).await?;
                        Ok(Some(()))
                    },
                    // The timer was aborted, do nothing.
                    Err(Aborted) => Ok(Some(())),
                }
            },
            recv_from_res = self.socket.recv_from(buf).fuse() => {
                let (size, _): (usize, SocketAddr) = recv_from_res.map_err(ClientError::Io)?;
                let () = self.handle_message_recv(&buf[..size]).await?;
                Ok(Some(()))
            },
            request = self.request_stream.next() => {
                match request {
                    Some(request_res) => {
                        let () = self.handle_client_request(request_res.map_err(ClientError::Fidl)?)?;
                        Ok(Some(()))
                    }
                    None => Ok(None),
                }
            }
        }
    }
}

/// Creates a socket listening on the input address.
async fn create_socket(addr: SocketAddr) -> Result<fasync::net::UdpSocket> {
    let socket = socket2::Socket::new(
        socket2::Domain::ipv6(),
        socket2::Type::dgram(),
        Some(socket2::Protocol::udp()),
    )?;
    // It is possible to run multiple clients on the same address.
    let () = socket.set_reuse_port(true)?;
    let () = socket.bind(&addr.into())?;
    fasync::net::UdpSocket::from_socket(socket.into_udp_socket()).context("converting socket")
}

/// Creates a transaction ID that can be used by the client as defined in [RFC 8415, Section 16.1].
///
/// [RFC 8415, Section 16.1]: https://tools.ietf.org/html/rfc8415#section-16.1
fn transaction_id() -> [u8; 3] {
    let mut id = [0u8; 3];
    let () = thread_rng().fill(&mut id[..]);
    id
}

/// Returns `true` if the input address is a link-local address (`fe80::/64`).
///
/// TODO(https://github.com/rust-lang/rust/issues/27709): use is_unicast_link_local_strict() in
/// stable rust when it's available.
fn is_unicast_link_local_strict(addr: &fnet::Ipv6Address) -> bool {
    addr.addr[..8] == [0xfe, 0x80, 0, 0, 0, 0, 0, 0]
}

/// Starts a client based on `params`.
///
/// `request_stream` will be serviced by the client.
pub(crate) async fn start_client(
    params: NewClientParams,
    request: ServerEnd<ClientMarker>,
) -> Result<()> {
    if let NewClientParams {
        interface_id: Some(interface_id),
        address: Some(address),
        models: Some(models),
    } = params
    {
        if Ipv6Addr::from(address.address.addr).is_multicast()
            || (is_unicast_link_local_strict(&address.address)
                && address.zone_index != interface_id)
        {
            return request
                .close_with_epitaph(zx::Status::INVALID_ARGS)
                .context("closing request channel with epitaph");
        }

        let fnetext::SocketAddress(addr) = fnet::SocketAddress::Ipv6(address).into();
        let mut client = Dhcpv6Client::start(
            transaction_id(),
            models,
            create_socket(addr).await?,
            SocketAddr::new(ALL_DHCP_AGENTS_AND_SERVERS, DHCPV6_AGENT_AND_SERVER_PORT),
            request.into_stream().context("getting new client request stream from channel")?,
        )
        .await?;
        let mut buf = vec![0u8; MAX_UDP_DATAGRAM_SIZE];
        loop {
            match client.handle_next_event(&mut buf).await? {
                Some(()) => (),
                None => break Ok(()),
            }
        }
    } else {
        // All param fields are required.
        request
            .close_with_epitaph(zx::Status::INVALID_ARGS)
            .context("closing request channel with epitaph")
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        dhcpv6::protocol::{Dhcpv6MessageBuilder, Dhcpv6MessageType},
        fidl::endpoints::create_endpoints,
        fidl_fuchsia_net_name as fnetname,
        futures::join,
        matches::assert_matches,
        net_declare::{fidl_ip_v6, fidl_socket_addr_v6, std_socket_addr},
        packet::serialize::InnerPacketBuilder,
        std::{collections::HashSet, task::Poll},
    };

    /// Creates a test socket bound to an ephemeral port on localhost.
    fn create_test_socket() -> (fasync::net::UdpSocket, SocketAddr) {
        let addr: SocketAddr = std_socket_addr!([::1]:0);
        let socket = std::net::UdpSocket::bind(addr).expect("failed to create test socket");
        let addr = socket.local_addr().expect("failed to get address of test socket");
        (fasync::net::UdpSocket::from_socket(socket).expect("failed to create test socket"), addr)
    }

    /// Asserts `socket` receives an information request from `want_from_addr`.
    async fn assert_received_information_request(
        socket: &fasync::net::UdpSocket,
        want_from_addr: SocketAddr,
    ) {
        let mut buf = vec![0u8; MAX_UDP_DATAGRAM_SIZE];
        let (size, from_addr) =
            socket.recv_from(&mut buf).await.expect("failed to receive on test server socket");
        assert_eq!(from_addr, want_from_addr);
        let buf = &mut &buf[..size]; // Implements BufferView.
        assert_matches!(
            Dhcpv6Message::parse(buf, ()),
            Ok(Dhcpv6Message { msg_type: Dhcpv6MessageType::InformationRequest, .. })
        )
    }

    #[test]
    fn test_create_client_with_unsupported_models() {
        assert_matches!(
            create_state_machine([1, 2, 3], OperationalModels::empty()),
            Err(ClientError::UnsupportedModels(OperationalModels { stateless: None }))
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_client_stops_on_channel_close() {
        let (client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_proxy = client_end.into_proxy().expect("failed to create test client proxy");

        let ((), client_res) = join!(
            async { drop(client_proxy) },
            start_client(
                NewClientParams {
                    interface_id: Some(1),
                    address: Some(fidl_socket_addr_v6!([::1]:546)),
                    models: Some(OperationalModels {
                        stateless: Some(Stateless { options_to_request: None })
                    }),
                },
                server_end,
            ),
        );
        client_res.expect("client future should return with Ok");
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_client_should_return_error_on_double_watch() {
        let (client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_proxy = client_end.into_proxy().expect("failed to create test client proxy");

        let (caller1_res, caller2_res, client_res) = join!(
            client_proxy.watch_servers(),
            client_proxy.watch_servers(),
            start_client(
                NewClientParams {
                    interface_id: Some(1),
                    address: Some(fidl_socket_addr_v6!([::1]:546)),
                    models: Some(OperationalModels {
                        stateless: Some(Stateless { options_to_request: None })
                    }),
                },
                server_end,
            )
        );

        assert_matches!(
            caller1_res,
            Err(fidl::Error::ClientChannelClosed(zx::Status::PEER_CLOSED))
        );
        assert_matches!(
            caller2_res,
            Err(fidl::Error::ClientChannelClosed(zx::Status::PEER_CLOSED))
        );
        assert!(client_res
            .expect_err("client should fail with double watch error")
            .to_string()
            .contains("got watch request while the previous one is pending"));
    }

    #[test]
    fn test_client_starts_with_valid_args() {
        let mut exec = fasync::Executor::new().expect("failed to create test executor");

        let (client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_proxy = client_end.into_proxy().expect("failed to create test client proxy");

        let test_fut = async {
            join!(
                client_proxy.watch_servers(),
                start_client(
                    NewClientParams {
                        interface_id: Some(1),
                        address: Some(fidl_socket_addr_v6!([::1]:546)),
                        models: Some(OperationalModels {
                            stateless: Some(Stateless { options_to_request: None })
                        }),
                    },
                    server_end
                )
            )
        };
        futures::pin_mut!(test_fut);
        assert_matches!(exec.run_until_stalled(&mut test_fut), Poll::Pending);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_client_fails_to_start_with_invalid_args() {
        for params in vec![
            // Missing required field.
            NewClientParams {
                interface_id: Some(1),
                address: None,
                models: Some(OperationalModels {
                    stateless: Some(Stateless { options_to_request: None }),
                }),
            },
            // Interface ID and zone index mismatch on link-local address.
            NewClientParams {
                interface_id: Some(2),
                address: Some(fnet::Ipv6SocketAddress {
                    address: fidl_ip_v6!(fe80::1),
                    port: DEFAULT_CLIENT_PORT,
                    zone_index: 1,
                }),
                models: Some(OperationalModels {
                    stateless: Some(Stateless { options_to_request: None }),
                }),
            },
            // Multicast address is invalid.
            NewClientParams {
                interface_id: Some(1),
                address: Some(fnet::Ipv6SocketAddress {
                    address: fidl_ip_v6!(ff01::1),
                    port: DEFAULT_CLIENT_PORT,
                    zone_index: 1,
                }),
                models: Some(OperationalModels {
                    stateless: Some(Stateless { options_to_request: None }),
                }),
            },
        ] {
            let (client_end, server_end) =
                create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
            let () =
                start_client(params, server_end).await.expect("start server failed unexpectedly");
            // Calling any function on the client proxy should fail due to channel closed with
            // `INVALID_ARGS`.
            assert_matches!(
                client_end.into_proxy().expect("failed to create test proxy").watch_servers().await,
                Err(fidl::Error::ClientChannelClosed(zx::Status::INVALID_ARGS))
            );
        }
    }

    #[test]
    fn test_is_unicast_link_local_strict() {
        assert_eq!(is_unicast_link_local_strict(&fidl_ip_v6!(fe80::)), true);
        assert_eq!(is_unicast_link_local_strict(&fidl_ip_v6!(fe80::1)), true);
        assert_eq!(is_unicast_link_local_strict(&fidl_ip_v6!(fe80::ffff:1:2:3)), true);
        assert_eq!(is_unicast_link_local_strict(&fidl_ip_v6!(fe80::1:0:0:0:0)), false);
        assert_eq!(is_unicast_link_local_strict(&fidl_ip_v6!(fe81::)), false);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_client_schedule_and_cancel_timers() {
        let (_client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_stream = server_end.into_stream().expect("failed to create test request stream");

        let (client_socket, _client_addr) = create_test_socket();
        let (_server_socket, server_addr) = create_test_socket();
        let mut client: Dhcpv6Client = Dhcpv6Client::start(
            [1, 2, 3],
            OperationalModels { stateless: Some(Stateless { options_to_request: None }) },
            client_socket,
            server_addr,
            client_stream,
        )
        .await
        .expect("creating test client");

        // Stateless DHCP client starts by scheduling a retransmission timer.
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        let () = client
            .cancel_timer(Dhcpv6ClientTimerType::Retransmission)
            .expect("canceling retransmission timer on test client");
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            Vec::<&Dhcpv6ClientTimerType>::new()
        );

        let () = client
            .schedule_timer(Dhcpv6ClientTimerType::Refresh, Duration::from_nanos(1))
            .expect("scheduling refresh timer on test client");
        let () = client
            .schedule_timer(Dhcpv6ClientTimerType::Retransmission, Duration::from_nanos(2))
            .expect("scheduling retransmission timer on test client");
        assert_eq!(
            client.timer_abort_handles.keys().collect::<HashSet<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission, &Dhcpv6ClientTimerType::Refresh]
                .into_iter()
                .collect()
        );

        assert_matches!(
            client.schedule_timer(Dhcpv6ClientTimerType::Refresh, Duration::from_nanos(1)),
            Err(ClientError::TimerAlreadyExist(Dhcpv6ClientTimerType::Refresh))
        );
        assert_matches!(
            client.schedule_timer(Dhcpv6ClientTimerType::Retransmission, Duration::from_nanos(2)),
            Err(ClientError::TimerAlreadyExist(Dhcpv6ClientTimerType::Retransmission))
        );

        let () = client
            .cancel_timer(Dhcpv6ClientTimerType::Refresh)
            .expect("canceling retransmission timer on test client");
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        assert_matches!(
            client.cancel_timer(Dhcpv6ClientTimerType::Refresh),
            Err(ClientError::MissingTimer(Dhcpv6ClientTimerType::Refresh))
        );

        let () = client
            .cancel_timer(Dhcpv6ClientTimerType::Retransmission)
            .expect("canceling retransmission timer on test client");
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<&Dhcpv6ClientTimerType>>(),
            Vec::<&Dhcpv6ClientTimerType>::new()
        );

        assert_matches!(
            client.cancel_timer(Dhcpv6ClientTimerType::Retransmission),
            Err(ClientError::MissingTimer(Dhcpv6ClientTimerType::Retransmission))
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_handle_next_event_on_client() {
        let (client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_proxy = client_end.into_proxy().expect("failed to create test client proxy");
        let client_stream = server_end.into_stream().expect("failed to create test request stream");

        let (client_socket, client_addr) = create_test_socket();
        let (server_socket, server_addr) = create_test_socket();
        let mut client: Dhcpv6Client = Dhcpv6Client::start(
            [1, 2, 3],
            OperationalModels { stateless: Some(Stateless { options_to_request: None }) },
            client_socket,
            server_addr,
            client_stream,
        )
        .await
        .expect("creating test client");

        // Starting the client in stateless should send an information request out.
        assert_received_information_request(&server_socket, client_addr).await;
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        let mut buf = vec![0u8; MAX_UDP_DATAGRAM_SIZE];
        // Trigger a retransmission.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        assert_received_information_request(&server_socket, client_addr).await;
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        // Message targeting another transaction ID should be ignored.
        let builder = Dhcpv6MessageBuilder::new(Dhcpv6MessageType::Reply, [5, 6, 7], &[]);
        let mut buf = vec![0u8; builder.bytes_len()];
        let () = builder.serialize(&mut buf);
        let _: usize = server_socket
            .send_to(&buf, client_addr)
            .await
            .expect("failed to send test message to test socket");
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        // Message targeting this client should cause the client to transition state.
        let builder = Dhcpv6MessageBuilder::new(Dhcpv6MessageType::Reply, [1, 2, 3], &[]);
        let mut buf = vec![0u8; builder.bytes_len()];
        let () = builder.serialize(&mut buf);
        let _: usize = server_socket
            .send_to(&buf, client_addr)
            .await
            .expect("failed to send test message to test socket");
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Refresh]
        );
        // Discard aborted retransmission timer.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));

        // Reschedule a shorter timer for Refresh so we don't spend time waiting in test.
        client
            .cancel_timer(Dhcpv6ClientTimerType::Refresh)
            .expect("failed to cancel timer on test client");
        // Discard cancelled refresh timer.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        client
            .schedule_timer(Dhcpv6ClientTimerType::Refresh, Duration::from_nanos(1))
            .expect("failed to schedule timer on test client");

        // Trigger a refresh.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        assert_received_information_request(&server_socket, client_addr).await;
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        // Test FIDL client requests are propagated.
        let test_sockaddr = fnet::SocketAddress::Ipv6(fnet::Ipv6SocketAddress {
            address: fnet::Ipv6Address { addr: [42; 16] },
            port: 321,
            zone_index: 42,
        });
        let test_fut = async {
            assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
            client
                .dns_responder
                .take()
                .expect("test client did not get a channel responder")
                .send(&mut std::iter::once(fnetname::DnsServer_ {
                    address: Some(test_sockaddr),
                    source: Some(fnetname::DnsServerSource::Dhcpv6(
                        fnetname::Dhcpv6DnsServerSource { source_interface: Some(42) },
                    )),
                }))
                .expect("failed to send response on test channel");
        };
        let (watcher_res, ()) = join!(client_proxy.watch_servers(), test_fut);
        let servers = watcher_res.expect("failed to watch servers");
        assert_eq!(
            servers,
            vec![fnetname::DnsServer_ {
                address: Some(test_sockaddr),
                source: Some(fnetname::DnsServerSource::Dhcpv6(fnetname::Dhcpv6DnsServerSource {
                    source_interface: Some(42)
                },)),
            }]
        );

        // Drop the channel should cause `handle_next_event(&mut buf)` to return `None`.
        drop(client_proxy);
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(None));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_handle_next_event_respects_timer_order() {
        let (_client_end, server_end) =
            create_endpoints::<ClientMarker>().expect("failed to create test fidl channel");
        let client_stream = server_end.into_stream().expect("failed to create test request stream");

        let (client_socket, client_addr) = create_test_socket();
        let (server_socket, server_addr) = create_test_socket();
        let mut client: Dhcpv6Client = Dhcpv6Client::start(
            [1, 2, 3],
            OperationalModels { stateless: Some(Stateless { options_to_request: None }) },
            client_socket,
            server_addr,
            client_stream,
        )
        .await
        .expect("creating test client");

        let mut buf = vec![0u8; MAX_UDP_DATAGRAM_SIZE];
        // A retransmission timer is scheduled when starting the client in stateless mode. Cancel
        // it and create a new one with a longer timeout so the test is not flaky.
        let () = client
            .cancel_timer(Dhcpv6ClientTimerType::Retransmission)
            .expect("failed to cancel timer on test client");
        // Discard cancelled retransmission timer.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        let () = client
            .schedule_timer(Dhcpv6ClientTimerType::Retransmission, Duration::from_secs(1_000_000))
            .expect("failed to schedule timer on test client");
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        // Trigger a message receive, the message is later discarded because transaction ID doesn't
        // match.
        let builder = Dhcpv6MessageBuilder::new(Dhcpv6MessageType::Reply, [5, 6, 7], &[]);
        let mut buf = vec![0u8; builder.bytes_len()];
        let () = builder.serialize(&mut buf);
        let _: usize = server_socket
            .send_to(&buf, client_addr)
            .await
            .expect("failed to send test message to test socket");
        // There are now two pending events, the message receive is handled first because the timer
        // is far into the future.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        // The retransmission timer is still here.
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );

        // Inserts a refresh timer that precedes the retransmission.
        let () = client
            .schedule_timer(Dhcpv6ClientTimerType::Refresh, Duration::from_nanos(1))
            .expect("scheduling refresh timer on test client");
        // This timer is scheduled.
        assert_eq!(
            client.timer_abort_handles.keys().collect::<HashSet<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission, &Dhcpv6ClientTimerType::Refresh]
                .into_iter()
                .collect()
        );

        // Now handle_next_event(&mut buf) should trigger a refresh because it precedes
        // retransmission.
        assert_matches!(client.handle_next_event(&mut buf).await, Ok(Some(())));
        // The refresh timer is consumed.
        assert_eq!(
            client.timer_abort_handles.keys().collect::<Vec<_>>(),
            vec![&Dhcpv6ClientTimerType::Retransmission]
        );
    }
}

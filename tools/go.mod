module go.fuchsia.dev/fuchsia/tools

go 1.15

require (
	cloud.google.com/go v0.65.0
	github.com/creack/pty v1.1.11
	github.com/go-yaml/yaml v2.1.0+incompatible
	github.com/golang/glog v0.0.0-20160126235308-23def4e6c14b
	github.com/golang/groupcache v0.0.0-20200121045136-8c9f03a8e57e // indirect
	github.com/golang/mock v1.4.4 // indirect
	github.com/golang/protobuf v1.4.2
	github.com/google/go-cmp v0.5.3-0.20201020212313-ab46b8bd0abd
	github.com/google/shlex v0.0.0-20191202100458-e7afc7fbc510
	github.com/google/subcommands v1.2.0
	github.com/hashicorp/golang-lru v0.5.4 // indirect
	github.com/julienschmidt/httprouter v1.3.0 // indirect
	github.com/kr/fs v0.1.0
	github.com/kr/pretty v0.1.0
	github.com/pkg/sftp v1.12.0
	github.com/smartystreets/assertions v1.2.0 // indirect
	github.com/smartystreets/goconvey v1.6.4 // indirect
	go.chromium.org/luci v0.0.0-20200828094204-41b976d93de1
	go.fuchsia.dev/fuchsia/garnet v0.0.0-20200821151753-3226fa91b98e
	go.fuchsia.dev/fuchsia/src v0.0.0-20200821151753-3226fa91b98e
	golang.org/x/crypto v0.0.0-20210921155107-089bfa567519
	golang.org/x/net v0.0.0-20220722155237-a158d28d115b
	golang.org/x/sync v0.0.0-20220722155255-886fb9371eb4
	golang.org/x/sys v0.0.0-20220722155257-8c9f86f7a55f
	golang.org/x/text v0.3.8 // indirect
	google.golang.org/api v0.30.0 // indirect
	google.golang.org/genproto v0.0.0-20200825200019-8632dd797987 // indirect
	google.golang.org/grpc v1.31.1 // indirect
	google.golang.org/protobuf v1.25.0
	honnef.co/go/tools v0.0.1-2020.1.5 // indirect
)

replace (
	cloud.google.com/go => ../third_party/golibs/github.com/googleapis/google-cloud-go
	github.com/creack/pty => ../third_party/golibs/github.com/creack/pty
	github.com/flynn/go-tuf => ../third_party/golibs/github.com/flynn/go-tuf
	github.com/go-yaml/yaml => ../third_party/golibs/github.com/go-yaml/yaml
	github.com/golang/protobuf => ../third_party/golibs/github.com/golang/protobuf
	github.com/google/go-cmp => ../third_party/golibs/github.com/google/go-cmp
	github.com/google/subcommands => ../third_party/golibs/github.com/google/subcommands
	github.com/pkg/sftp => ../third_party/golibs/github.com/pkg/sftp
	go.fuchsia.dev/fuchsia/garnet => ../garnet
	go.fuchsia.dev/fuchsia/src => ../src
	golang.org/x/crypto => ../third_party/golibs/golang.org/x/crypto
	golang.org/x/net => ../third_party/golibs/golang.org/x/net
	golang.org/x/sync => ../third_party/golibs/golang.org/x/sync
	golang.org/x/sys => ../third_party/golibs/golang.org/x/sys
	google.golang.org/protobuf => ../third_party/golibs/github.com/protocolbuffers/protobuf-go
)

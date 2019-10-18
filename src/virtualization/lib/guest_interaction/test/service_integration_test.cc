// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/virtualization/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/vfs.h>
#include <lib/fzl/fdio.h>
#include <lib/sys/cpp/file_descriptor.h>
#include <unistd.h>

#include "fuchsia/netemul/guest/cpp/fidl.h"
#include "gtest/gtest.h"
#include "src/lib/fxl/logging.h"
#include "src/virtualization/lib/guest_interaction/common.h"
#include "src/virtualization/lib/guest_interaction/test/integration_test_lib.h"

// Call into the guest interaction fidl service to push a test bash script, run
// the bash script, and pull back the results generated by the script.  The
// script emits output to stdout and stderr and accepts input from stdin which
// is used to generate the output file.
TEST_F(GuestInteractionTest, FidlExecScriptTest) {
  // Add guest interaction service
  fuchsia::sys::LaunchInfo discovery_service_launch_info;
  discovery_service_launch_info.url = kGuestDiscoveryUrl;
  discovery_service_launch_info.out = sys::CloneFileDescriptor(1);
  discovery_service_launch_info.err = sys::CloneFileDescriptor(2);
  services_->AddServiceWithLaunchInfo(std::move(discovery_service_launch_info),
                                      fuchsia::netemul::guest::GuestDiscovery::Name_);

  // Create environment services and launch the Debian guest.
  CreateEnvironment();
  LaunchDebianGuest();

  // Guest interaction boilerplate
  fuchsia::netemul::guest::GuestDiscoveryPtr gds;
  env_->ConnectToService(gds.NewRequest());

  fuchsia::netemul::guest::GuestInteractionPtr gis;
  gds->GetGuest(nullptr, kGuestLabel, gis.NewRequest());

  ASSERT_TRUE(gis.is_bound());

  // Push the bash script to the guest
  fidl::InterfaceHandle<fuchsia::io::File> put_file;
  zx_status_t status = fdio_open(kTestScriptSource, ZX_FS_RIGHT_READABLE,
                                 put_file.NewRequest().TakeChannel().release());

  ASSERT_EQ(status, ZX_OK);

  bool put_complete = false;
  zx_status_t transfer_status = ZX_ERR_BAD_STATE;
  gis->PutFile(std::move(put_file), kGuestScriptDestination,
               [&transfer_status, &put_complete](zx_status_t put_result) {
                 transfer_status = put_result;
                 put_complete = true;
               });
  RunLoopUntil([&put_complete]() { return put_complete; });
  ASSERT_EQ(transfer_status, ZX_OK);

  // Run the bash script in the guest.  The script will write to stdout and
  // stderr.  The script will also block waiting to receive input from stdin.
  zx::socket stdin_writer, stdin_reader;
  zx::socket::create(0, &stdin_writer, &stdin_reader);

  zx::socket stdout_writer, stdout_reader;
  zx::socket::create(0, &stdout_writer, &stdout_reader);

  zx::socket stderr_writer, stderr_reader;
  zx::socket::create(0, &stderr_writer, &stderr_reader);

  bool exec_started = false;
  bool exec_terminated = false;

  // Run the bash script on the guest.
  std::string command = std::string("/bin/sh ");
  command.append(kGuestScriptDestination);
  std::vector<fuchsia::netemul::guest::EnvironmentVariable> env_vars = {
      {"STDOUT_STRING", kTestStdout}, {"STDERR_STRING", kTestStderr}};
  std::string std_out;
  std::string std_err;
  zx_status_t exec_started_status = ZX_ERR_BAD_STATE;
  zx_status_t exec_terminated_status = ZX_ERR_BAD_STATE;

  fuchsia::netemul::guest::CommandListenerPtr listener;
  listener.events().OnStarted = [&](zx_status_t exec_status) {
    // Once the subprocess has started, write to stdin.
    std::string to_write = std::string(::kTestScriptInput);
    uint32_t bytes_written = 0;

    while (bytes_written < to_write.size()) {
      size_t curr_bytes_written;
      zx_status_t write_status =
          stdin_writer.write(0, &(to_write.c_str()[bytes_written]), to_write.size() - bytes_written,
                             &curr_bytes_written);
      if (write_status != ZX_OK) {
        return;
      }

      bytes_written += curr_bytes_written;
    }
    stdin_writer.shutdown(ZX_SOCKET_SHUTDOWN_MASK);
    exec_started_status = status;
    exec_started = true;
  };
  listener.events().OnTerminated = [&](zx_status_t exec_result, int32_t exit_code) {
    // When the process terminates, read from stdout and stderr.
    exec_terminated_status = exec_result;

    size_t bytes_read = 0;
    char read_buf[100];
    while (true) {
      zx_status_t read_status = stdout_reader.read(0, read_buf, 100, &bytes_read);
      if (read_status != ZX_OK) {
        break;
      }
      std_out += std::string(read_buf, bytes_read);
    }
    while (true) {
      zx_status_t read_status = stderr_reader.read(0, read_buf, 100, &bytes_read);
      if (read_status != ZX_OK) {
        break;
      }
      std_err += std::string(read_buf, bytes_read);
    }
    exec_terminated = true;
  };

  gis->ExecuteCommand(command, env_vars, std::move(stdin_reader), std::move(stdout_writer),
                      std::move(stderr_writer), listener.NewRequest());

  // Wait for the subprocess to start.
  RunLoopUntil([&exec_started]() { return exec_started; });
  ASSERT_EQ(exec_started_status, ZX_OK);

  // Wait for the bash script to finish.
  RunLoopUntil([&exec_terminated]() { return exec_terminated; });

  // Validate the stdout and stderr.
  ASSERT_EQ(exec_terminated_status, ZX_OK);
  ASSERT_EQ(std_out, (std::string(kTestStdout) + std::string("\n")));
  ASSERT_EQ(std_err, (std::string(kTestStderr) + std::string("\n")));

  // The bash script will create a file with contents that were written to
  // stdin.  Pull this file back and inspect its contents.
  fidl::InterfaceHandle<fuchsia::io::File> get_file;
  status = fdio_open(kHostOuputCopyLocation,
                     ZX_FS_RIGHT_WRITABLE | ZX_FS_FLAG_CREATE | ZX_FS_FLAG_TRUNCATE,
                     get_file.NewRequest().TakeChannel().release());
  ASSERT_EQ(status, ZX_OK);

  bool get_complete = false;
  transfer_status = ZX_ERR_BAD_STATE;
  gis->GetFile(kGuestFileOutputLocation, std::move(get_file),
               [&transfer_status, &get_complete](zx_status_t get_result) {
                 transfer_status = get_result;
                 get_complete = true;
               });
  RunLoopUntil([&get_complete]() { return get_complete; });

  // Verify the contents that were communicated through stdin.
  std::string output_string;
  char output_buf[100];
  int fd = open(kHostOuputCopyLocation, O_RDONLY);
  while (true) {
    size_t read_size = read(fd, output_buf, 100);
    if (read_size <= 0)
      break;
    output_string.append(std::string(output_buf, read_size));
  }
  close(fd);

  ASSERT_EQ(output_string, std::string(kTestScriptInput));
}

TEST_F(GuestInteractionTest, FidlPutGetTest) {
  // Add guest interaction service
  fuchsia::sys::LaunchInfo discovery_service_launch_info;
  discovery_service_launch_info.url = kGuestDiscoveryUrl;
  discovery_service_launch_info.out = sys::CloneFileDescriptor(1);
  discovery_service_launch_info.err = sys::CloneFileDescriptor(2);
  services_->AddServiceWithLaunchInfo(std::move(discovery_service_launch_info),
                                      fuchsia::netemul::guest::GuestDiscovery::Name_);

  // Create environment services and launch the Debian guest.
  CreateEnvironment();
  LaunchDebianGuest();

  // Guest interaction boilerplate
  fuchsia::netemul::guest::GuestDiscoveryPtr gds;
  env_->ConnectToService(gds.NewRequest());

  fuchsia::netemul::guest::GuestInteractionPtr gis;
  gds->GetGuest(nullptr, kGuestLabel, gis.NewRequest());

  // Write a file of gibberish that the test can send over to the guest.
  char test_file[] = "/data/test_file.txt";
  char guest_destination[] = "/root/new/directory/test_file.txt";
  char host_verification_file[] = "/data/verification_file.txt";

  std::string file_contents;
  for (int i = 0; i < 2 * CHUNK_SIZE; i++) {
    file_contents.push_back(i % (('z' - 'A') + 'A'));
  }
  uint32_t fd = open(test_file, O_WRONLY | O_TRUNC | O_CREAT);
  uint32_t bytes_written = 0;
  while (bytes_written < file_contents.size()) {
    ssize_t write_size =
        write(fd, file_contents.c_str() + bytes_written, file_contents.size() - bytes_written);
    ASSERT_TRUE(write_size > 0);
    bytes_written += write_size;
  }

  // Push the file to the guest
  fidl::InterfaceHandle<fuchsia::io::File> put_file;
  zx_status_t status =
      fdio_open(test_file, ZX_FS_RIGHT_READABLE, put_file.NewRequest().TakeChannel().release());
  ASSERT_EQ(status, ZX_OK);

  bool put_complete = false;
  zx_status_t transfer_status = ZX_ERR_BAD_STATE;
  gis->PutFile(std::move(put_file), guest_destination,
               [&transfer_status, &put_complete](zx_status_t put_result) {
                 transfer_status = put_result;
                 put_complete = true;
               });
  RunLoopUntil([&put_complete]() { return put_complete; });
  close(fd);
  ASSERT_EQ(transfer_status, ZX_OK);

  // Pull back the guest's copy of the file and ensure the contents match those
  // from the file generated above.
  fidl::InterfaceHandle<fuchsia::io::File> get_file;
  status = fdio_open(host_verification_file,
                     ZX_FS_RIGHT_WRITABLE | ZX_FS_FLAG_CREATE | ZX_FS_FLAG_TRUNCATE,
                     get_file.NewRequest().TakeChannel().release());

  bool get_complete = false;
  transfer_status = ZX_ERR_BAD_STATE;
  gis->GetFile(guest_destination, std::move(get_file),
               [&transfer_status, &get_complete](zx_status_t get_result) {
                 transfer_status = get_result;
                 get_complete = true;
               });
  RunLoopUntil([&get_complete]() { return get_complete; });
  ASSERT_EQ(transfer_status, ZX_OK);

  // Verify the contents that were communicated through stdin.
  std::string output_string;
  char output_buf[100];
  fd = open(host_verification_file, O_RDONLY);
  while (true) {
    size_t read_size = read(fd, output_buf, 100);
    if (read_size <= 0)
      break;
    output_string.append(std::string(output_buf, read_size));
  }

  ASSERT_EQ(output_string, file_contents);
}

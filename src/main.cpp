#include "kr260demo/rpu_control_protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr auto kRpmsgBus = "/sys/bus/rpmsg";
constexpr auto kDevDir = "/dev";
constexpr int kDefaultTimeoutMs = 1000;

struct Target {
	std::string name;
	std::string service;
};

struct UniqueFd {
	explicit UniqueFd(int fd = -1) : fd(fd) {}
	~UniqueFd()
	{
		if (fd >= 0)
			::close(fd);
	}

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;

	UniqueFd(UniqueFd &&other) noexcept : fd(other.fd)
	{
		other.fd = -1;
	}

	UniqueFd &operator=(UniqueFd &&other) noexcept
	{
		if (this == &other)
			return *this;
		if (fd >= 0)
			::close(fd);
		fd = other.fd;
		other.fd = -1;
		return *this;
	}

	int get() const { return fd; }

private:
	int fd;
};

[[noreturn]] void fail_errno(const std::string &what)
{
	throw std::runtime_error(what + ": " + std::strerror(errno));
}

std::string trim(std::string value)
{
	while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
				  value.back() == '\t' || value.back() == ' '))
		value.pop_back();
	while (!value.empty() && (value.front() == '\n' || value.front() == '\r' ||
				  value.front() == '\t' || value.front() == ' '))
		value.erase(value.begin());
	return value;
}

std::string read_text(const fs::path &path)
{
	std::ifstream input(path);
	if (!input)
		fail_errno("open " + path.string());
	return std::string(std::istreambuf_iterator<char>(input), {});
}

void write_sysfs(const fs::path &path, std::string_view value, bool include_null = false)
{
	UniqueFd fd(::open(path.c_str(), O_WRONLY));
	if (fd.get() < 0)
		fail_errno("open " + path.string());

	const auto len = include_null ? value.size() + 1 : value.size();
	if (::write(fd.get(), value.data(), len) != static_cast<ssize_t>(len))
		fail_errno("write " + path.string());
}

std::vector<fs::directory_entry> sorted_dir(const fs::path &path)
{
	std::vector<fs::directory_entry> entries;
	for (const auto &entry : fs::directory_iterator(path))
		entries.push_back(entry);
	std::sort(entries.begin(), entries.end(),
		  [](const auto &lhs, const auto &rhs) {
			  return lhs.path().filename().string() < rhs.path().filename().string();
		  });
	return entries;
}

std::string lookup_channel(const std::string &service)
{
	const fs::path devices = fs::path(kRpmsgBus) / "devices";
	if (!fs::exists(devices))
		throw std::runtime_error("rpmsg bus not found at " + devices.string());

	for (const auto &entry : sorted_dir(devices)) {
		if (!entry.is_directory())
			continue;
		const auto name = entry.path().filename().string();
		if (name.find(service) != std::string::npos)
			return name;

		const fs::path name_path = entry.path() / "name";
		if (fs::exists(name_path) && trim(read_text(name_path)) == service)
			return name;
	}

	throw std::runtime_error("no rpmsg channel for service '" + service + "'");
}

void bind_rpmsg_chrdev(const std::string &rpmsg_device)
{
	const fs::path dev_path = fs::path(kRpmsgBus) / "devices" / rpmsg_device;
	const fs::path rpmsg_dir = dev_path / "rpmsg";
	if (fs::exists(rpmsg_dir))
		return;

	const fs::path override_path = dev_path / "driver_override";
	auto override = trim(read_text(override_path));
	if (override != "rpmsg_chrdev") {
		if (!override.empty() && override != "(null)")
			throw std::runtime_error(rpmsg_device + " is already bound to " + override);
		write_sysfs(override_path, "rpmsg_chrdev", true);
	}

	const fs::path bind_path = fs::path(kRpmsgBus) / "drivers" / "rpmsg_chrdev" / "bind";
	try {
		write_sysfs(bind_path, rpmsg_device, true);
	} catch (const std::runtime_error &) {
		if (!fs::exists(rpmsg_dir))
			throw;
	}
}

std::string find_data_device(const std::string &rpmsg_device,
			     const std::string &service)
{
	const fs::path rpmsg_dir = fs::path(kRpmsgBus) / "devices" / rpmsg_device / "rpmsg";
	if (!fs::exists(rpmsg_dir))
		throw std::runtime_error("rpmsg data directory not found: " + rpmsg_dir.string());

	for (const auto &entry : sorted_dir(rpmsg_dir)) {
		const auto name = entry.path().filename().string();
		if (name.rfind("rpmsg", 0) != 0 || name.rfind("rpmsg_ctrl", 0) == 0)
			continue;

		const fs::path name_path = entry.path() / "name";
		if (fs::exists(name_path) && trim(read_text(name_path)) == service)
			return name;
	}

	throw std::runtime_error("no rpmsg data device for service '" + service + "'");
}

std::string msg_type_name(uint8_t type)
{
	switch (type) {
	case KR260_RPU_MSG_PONG:
		return "pong";
	case KR260_RPU_MSG_STATUS:
		return "status";
	case KR260_RPU_MSG_ACK:
		return "ack";
	case KR260_RPU_MSG_ERROR:
		return "error";
	default:
		return "type-" + std::to_string(type);
	}
}

std::string led_mode_name(uint32_t mode)
{
	switch (mode) {
	case KR260_RPU_LED_OFF:
		return "off";
	case KR260_RPU_LED_ON:
		return "on";
	case KR260_RPU_LED_TOGGLE:
		return "toggle";
	case KR260_RPU_LED_HEARTBEAT:
		return "heartbeat";
	default:
		return "unknown";
	}
}

uint8_t parse_led_mode(const std::string &mode)
{
	if (mode == "off")
		return KR260_RPU_LED_OFF;
	if (mode == "on")
		return KR260_RPU_LED_ON;
	if (mode == "toggle")
		return KR260_RPU_LED_TOGGLE;
	if (mode == "heartbeat")
		return KR260_RPU_LED_HEARTBEAT;
	throw std::runtime_error("invalid LED mode '" + mode + "'");
}

class RpmsgEndpoint {
public:
	explicit RpmsgEndpoint(const Target &target)
		: target_(target)
	{
		const auto rpmsg_device = lookup_channel(target_.service);
		bind_rpmsg_chrdev(rpmsg_device);

		const auto endpoint_name = find_data_device(rpmsg_device,
							    target_.service);
		const fs::path dev_path = fs::path(kDevDir) / endpoint_name;
		data_fd_ = UniqueFd(::open(dev_path.c_str(), O_RDWR | O_NONBLOCK));
		if (data_fd_.get() < 0)
			fail_errno("open " + dev_path.string());
	}

	std::vector<uint8_t> transact(uint8_t request_type, const void *payload,
				      uint16_t payload_len, uint8_t expected_type,
				      int timeout_ms)
	{
		if (sizeof(kr260demo_rpu_msg_header) + payload_len > KR260_RPU_MAX_FRAME_SIZE)
			throw std::runtime_error("request frame is too large");

		const auto sequence = ++sequence_;
		std::array<uint8_t, KR260_RPU_MAX_FRAME_SIZE> request {};
		kr260demo_rpu_msg_header header {};
		header.magic = KR260_RPU_MAGIC;
		header.version = KR260_RPU_VERSION;
		header.type = request_type;
		header.payload_len = payload_len;
		header.sequence = sequence;
		header.status = KR260_RPU_STATUS_OK;
		std::memcpy(request.data(), &header, sizeof(header));
		if (payload_len)
			std::memcpy(request.data() + sizeof(header), payload, payload_len);

		const auto frame_len = sizeof(header) + payload_len;
		const auto written = ::write(data_fd_.get(), request.data(), frame_len);
		if (written != static_cast<ssize_t>(frame_len))
			fail_errno("write rpmsg request");

		pollfd pfd {};
		pfd.fd = data_fd_.get();
		pfd.events = POLLIN;
		const auto poll_result = ::poll(&pfd, 1, timeout_ms);
		if (poll_result < 0)
			fail_errno("poll rpmsg response");
		if (poll_result == 0)
			throw std::runtime_error("timeout waiting for " + target_.name);

		std::array<uint8_t, KR260_RPU_MAX_FRAME_SIZE> response {};
		const auto read_len = ::read(data_fd_.get(), response.data(), response.size());
		if (read_len < 0)
			fail_errno("read rpmsg response");
		if (read_len < static_cast<ssize_t>(sizeof(kr260demo_rpu_msg_header)))
			throw std::runtime_error("short response from " + target_.name);

		kr260demo_rpu_msg_header response_header {};
		std::memcpy(&response_header, response.data(), sizeof(response_header));
		if (response_header.magic != KR260_RPU_MAGIC)
			throw std::runtime_error("bad response magic from " + target_.name);
		if (response_header.version != KR260_RPU_VERSION)
			throw std::runtime_error("bad response version from " + target_.name);
		if (response_header.sequence != sequence)
			throw std::runtime_error("sequence mismatch from " + target_.name);
		if (response_header.payload_len + sizeof(response_header) !=
		    static_cast<size_t>(read_len))
			throw std::runtime_error("response length mismatch from " + target_.name);
		if (response_header.type == KR260_RPU_MSG_ERROR)
			throw std::runtime_error(target_.name + " returned error status " +
						 std::to_string(response_header.status));
		if (response_header.status != KR260_RPU_STATUS_OK)
			throw std::runtime_error(target_.name + " returned status " +
						 std::to_string(response_header.status));
		if (response_header.type != expected_type)
			throw std::runtime_error("expected " + msg_type_name(expected_type) +
						 " but got " +
						 msg_type_name(response_header.type));

		return std::vector<uint8_t>(response.begin() + sizeof(response_header),
					    response.begin() + read_len);
	}

private:
	Target target_;
	uint32_t sequence_ = 0;
	UniqueFd data_fd_;
};

std::vector<Target> targets_for(const std::string &name)
{
	const Target r5c0 {"r5c0", "mncos-r5c0-ctrl"};
	const Target r5c1 {"r5c1", "mncos-r5c1-ctrl"};

	if (name == "r5c0")
		return {r5c0};
	if (name == "r5c1")
		return {r5c1};
	if (name == "all")
		return {r5c0, r5c1};
	throw std::runtime_error("invalid target '" + name + "'");
}

bool is_target(const std::string &value)
{
	return value == "r5c0" || value == "r5c1" || value == "all";
}

void usage(const char *argv0)
{
	std::cerr
		<< "Usage:\n"
		<< "  " << argv0 << " ping [r5c0|r5c1|all] [--timeout-ms N]\n"
		<< "  " << argv0 << " status [r5c0|r5c1|all] [--timeout-ms N]\n"
		<< "  " << argv0 << " led [r5c0|r5c1|all] <off|on|toggle|heartbeat> [--timeout-ms N]\n";
}

int parse_timeout(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) != "--timeout-ms")
			continue;
		if (i + 1 >= argc)
			throw std::runtime_error("--timeout-ms requires a value");
		return std::stoi(argv[i + 1]);
	}
	return kDefaultTimeoutMs;
}

std::vector<std::string> positional_args(int argc, char **argv)
{
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--timeout-ms") {
			++i;
			continue;
		}
		args.emplace_back(argv[i]);
	}
	return args;
}

void print_status(const Target &target, const std::vector<uint8_t> &payload)
{
	if (payload.size() != sizeof(kr260demo_rpu_status_payload))
		throw std::runtime_error("bad status payload from " + target.name);

	kr260demo_rpu_status_payload status {};
	std::memcpy(&status, payload.data(), sizeof(status));
	std::cout << target.name << ": core=" << status.core_id
		  << " led=" << led_mode_name(status.led_mode)
		  << " led_on=" << status.led_on
		  << " heartbeat=" << status.heartbeat_count
		  << " uptime_ms=" << status.uptime_ms
		  << " rx=" << status.rx_count
		  << " errors=" << status.error_count << '\n';
}

int run(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	const auto timeout_ms = parse_timeout(argc, argv);
	const auto args = positional_args(argc, argv);
	if (args.empty()) {
		usage(argv[0]);
		return 2;
	}

	const auto &command = args[0];
	auto target_name = std::string("all");
	size_t index = 1;
	if (index < args.size() && is_target(args[index]))
		target_name = args[index++];

	const auto targets = targets_for(target_name);

	if (command == "ping") {
		if (index != args.size())
			throw std::runtime_error("unexpected argument '" + args[index] + "'");
		for (const auto &target : targets) {
			RpmsgEndpoint endpoint(target);
			endpoint.transact(KR260_RPU_MSG_PING, nullptr, 0,
					  KR260_RPU_MSG_PONG, timeout_ms);
			std::cout << target.name << ": pong\n";
		}
		return 0;
	}

	if (command == "status") {
		if (index != args.size())
			throw std::runtime_error("unexpected argument '" + args[index] + "'");
		for (const auto &target : targets) {
			RpmsgEndpoint endpoint(target);
			auto payload = endpoint.transact(KR260_RPU_MSG_GET_STATUS, nullptr, 0,
							 KR260_RPU_MSG_STATUS, timeout_ms);
			print_status(target, payload);
		}
		return 0;
	}

	if (command == "led") {
		if (index >= args.size())
			throw std::runtime_error("missing LED mode");
		const auto mode = parse_led_mode(args[index++]);
		if (index != args.size())
			throw std::runtime_error("unexpected argument '" + args[index] + "'");

		kr260demo_rpu_led_payload payload {};
		payload.mode = mode;
		for (const auto &target : targets) {
			RpmsgEndpoint endpoint(target);
			endpoint.transact(KR260_RPU_MSG_SET_LED, &payload, sizeof(payload),
					  KR260_RPU_MSG_ACK, timeout_ms);
			std::cout << target.name << ": led " << led_mode_name(mode) << '\n';
		}
		return 0;
	}

	throw std::runtime_error("unknown command '" + command + "'");
}

} // namespace

int main(int argc, char **argv)
{
	try {
		return run(argc, argv);
	} catch (const std::exception &error) {
		std::cerr << "apu-rpu-ctl: " << error.what() << '\n';
		return 1;
	}
}

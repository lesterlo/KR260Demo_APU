#include "kr260demo/rpu_control_protocol.h"

#include "mnc/rpmsg_chrdev.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultTimeoutMs = 1000;

struct Target {
	std::string name;
	std::string service;
};

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

/*
 * Protocol layer: frames kr260demo control requests over the raw rpmsg
 * char-device transport (mnc::RpmsgChrdev) and validates the responses.
 */
class RpmsgEndpoint {
public:
	explicit RpmsgEndpoint(const Target &target)
		: target_(target), channel_(target.service)
	{
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
		std::vector<uint8_t> response;
		try {
			response = channel_.transact(request.data(), frame_len,
						     KR260_RPU_MAX_FRAME_SIZE, timeout_ms);
		} catch (const mnc::TimeoutError &) {
			throw std::runtime_error("timeout waiting for " + target_.name);
		}

		if (response.size() < sizeof(kr260demo_rpu_msg_header))
			throw std::runtime_error("short response from " + target_.name);

		kr260demo_rpu_msg_header response_header {};
		std::memcpy(&response_header, response.data(), sizeof(response_header));
		if (response_header.magic != KR260_RPU_MAGIC)
			throw std::runtime_error("bad response magic from " + target_.name);
		if (response_header.version != KR260_RPU_VERSION)
			throw std::runtime_error("bad response version from " + target_.name);
		if (response_header.sequence != sequence)
			throw std::runtime_error("sequence mismatch from " + target_.name);
		if (response_header.payload_len + sizeof(response_header) != response.size())
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
					    response.end());
	}

private:
	Target target_;
	uint32_t sequence_ = 0;
	mnc::RpmsgChrdev channel_;
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

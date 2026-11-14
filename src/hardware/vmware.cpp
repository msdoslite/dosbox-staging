// SPDX-FileCopyrightText:  2023-2025 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "vmware.h"

#include "vmware_hgfs.h"
#include <algorithm>
#include <map>
#include <set>

#include "config/setup.h"
#include "cpu/registers.h"
#include "dosbox.h"
#include "hardware/input/mouse.h"
#include "hardware/port.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

// Reference:
// - https://wiki.osdev.org/VMware_tools
// Drivers:
// - https://git.javispedro.com/cgit/vbados.git
// - https://github.com/NattyNarwhal/vmwmouse (warning: release 0.1 is unstable)
// - official Windows 9x VMware mouse driver

static bool is_interface_enabled = false;
static bool has_feature_mouse    = false;

// Whether Intel 8042 entry point API is currently enabled
static bool is_i8042_unlocked = false;

// Currently running program
static std::string program_segment_name = {};

// Queued data, waiting to be fectehd by the guest side driver
static std::vector<uint32_t> abs_pointer_queue = {};

enum class QueueContent {
	None,
	CommandResponse,
	MouseStatus,
};

static QueueContent abs_pointer_queue_content = QueueContent::None;

// ***************************************************************************
// Various common constants and type definitions
// ***************************************************************************

static std::set<std::string> SegmentBlackList = {
	// JEMM memory manager assumes certain memory layout if it detects the
	// VMware interface; this leads to incorrect JEMM behavior
	"JEMM386",
	"JEMMEX"
};

// Magic number for all VMware calls
static constexpr uint32_t VmWareMagic = 0x564d5868u;

// Not 100% sure if the meaning is correct, values were deducted by observing
// the VMware Workstation Player behavior
static constexpr uint32_t VmWareUnimplemented = 0xffffffffu;
static constexpr uint32_t VmWareAcknowledged  = 0x01u;

static constexpr size_t VmWareMouseStatusSize = 4;

// The exact meaning of the version ID below is unknown - so far we know that:
// - Linux kernel requires precisely this particular version ID, otherwise
//   it refuses to talk to the VmWare mouse interface.
// - The official VMware mouse driver for Windows 9x seems to be doing a version
//   ID validation, too - you can't just provide any random value. Details are
//   unknown, but this particular value works.
static constexpr uint32_t VmMouseVersionId = 0x3442554au;

enum class VmWareCommand : uint16_t {
	GetVersion         = 10,
	AbsPointerData     = 39,
	AbsPointerStatus   = 40,
	AbsPointerCommand  = 41,
	AbsPointerRestrict = 86,
};

enum class VmMouseCommand : uint32_t {
	Enable   = 0x45414552u,
	Disable  = 0xf5u,
	Absolute = 0x53424152u,
	Relative = 0x4c455252u,
};

// ***************************************************************************
// Mouse queue and commands
// ***************************************************************************

static void clear_abs_pointer_queue()
{
	abs_pointer_queue.clear();
	abs_pointer_queue_content = QueueContent::None;
}

static uint32_t fetch_from_abs_pointer_queue()
{
	if (abs_pointer_queue.empty()) {
		return 0;
	}

	const auto result = abs_pointer_queue.back();
	abs_pointer_queue.pop_back();
	if (abs_pointer_queue.empty()) {
		abs_pointer_queue_content = QueueContent::None;
	}

	return result;
}

static void mouse_status_to_abs_pointer_queue()
{
	if (abs_pointer_queue_content == QueueContent::CommandResponse) {
		// Queue contains a command response, do not override it
		return;
	}

	if (abs_pointer_queue_content == QueueContent::MouseStatus &&
	    abs_pointer_queue.size() != VmWareMouseStatusSize) {
		// Queue contains a mouse status, which is already being read,
		// we can't override it right now withut distrupting the guest
		// side driver
		return;
	}

	if (!MOUSEVMM_CheckIfUpdated_VmWare()) {
		return;
	}

	MouseVmWarePointerStatus status = {};
	MOUSEVMM_GetPointerStatus(status);

	clear_abs_pointer_queue();
	abs_pointer_queue.push_back(status.wheel_counter);
	abs_pointer_queue.push_back(status.absolute_y);
	abs_pointer_queue.push_back(status.absolute_x);
	abs_pointer_queue.push_back(status.buttons);
	abs_pointer_queue_content = QueueContent::MouseStatus;
}

static void execute_command(const VmMouseCommand command)
{
	clear_abs_pointer_queue();
	switch (command) {
	case VmMouseCommand::Enable:
		// We can ignore it
		break;
	case VmMouseCommand::Absolute:
		MOUSEVMM_Activate(MouseVmmProtocol::VmWare);
		break;
	case VmMouseCommand::Disable:
	case VmMouseCommand::Relative:
		MOUSEVMM_Deactivate(MouseVmmProtocol::VmWare);
		break;
	default:
		LOG_WARNING("VMWARE: Unimplemented mouse subcommand 0x%08x", reg_ebx);
		break;
	}
}

// ***************************************************************************
// Low bandwidth I/O port interface
// ***************************************************************************

static void command_get_version()
{
	// This command is a common way to detect VMware - since currently we
	// only implement  mouse support, hide the interface from software which
	// is known to misbehave with our limited implementation
	if (!SegmentBlackList.contains(program_segment_name)) {
		reg_ebx = VmWareMagic;
		// Return same protocol version as the VMware Workstation Player
		reg_eax = 6;
		reg_ecx = 4;
	}
}

static void command_abs_pointer_data()
{
	if (abs_pointer_queue.empty() || abs_pointer_queue.size() == 4) {
		reg_eax = fetch_from_abs_pointer_queue();
		reg_ebx = fetch_from_abs_pointer_queue();
		reg_ecx = fetch_from_abs_pointer_queue();
		reg_edx = fetch_from_abs_pointer_queue();
	} else {
		// Should not happen with a properly functioning guest driver
		LOG_WARNING("VMWARE: No valid mouse pointer status in the queue");
		clear_abs_pointer_queue();
		reg_eax = 0;
		reg_ebx = 0;
		reg_ecx = 0;
		reg_edx = 0;
	}
}

static void command_abs_pointer_status()
{
	mouse_status_to_abs_pointer_queue();
	reg_eax = check_cast<uint32_t>(abs_pointer_queue.size());
}

static void command_abs_pointer()
{
	const auto command = static_cast<VmMouseCommand>(reg_ebx);

	if (command == VmMouseCommand::Enable) {
		// For the standard VMware port interface we need regular PS/2
		// auxilary (mouse) interrupt handling
		MOUSEVMM_EnableImmediateInterrupts(false);
	}

	execute_command(command);
}

// Prototype HGFS transport. Each channel owns its transfer state; the TCLO
// timer can interleave with a filesystem request on RPCI.
struct HgfsChannel {
	uint16_t id                  = 0;
	uint32_t type                = 0;
	uint32_t expected            = 0;
	size_t cursor                = 0;
	std::vector<uint8_t> request = {}, reply = {};
};
static bool hgfs_enabled                             = false;
static bool hgfs_trace                               = false;
static std::map<uint16_t, HgfsChannel> hgfs_channels = {};
static uint16_t hgfs_next_channel                    = 1;
static void hgfs_dispatch(HgfsChannel& c)
{
	c.reply  = c.type == 0x49435052 ? VmwareHgfs::Request(c.request, c.id)
	                                : std::vector<uint8_t>{};
	c.cursor = 0;
	if (hgfs_trace && c.type == 0x49435052 && c.request.size() >= 10 &&
	    c.request[0] == 'f' && c.request[1] == ' ') {
		const auto op = c.request[6];
		if (op != 25) {
			LOG_MSG("HGFS: op=%u request=%zu reply=%zu status=%u",
			        op,
			        c.request.size(),
			        c.reply.size(),
			        c.reply.size() >= 10 ? c.reply[6] : 255);
		}
	}
}
static void hgfs_command()
{
	const auto command = reg_ecx >> 16;
	reg_eax            = 0;
	if (command == 0) {
		const auto type = reg_ebx & 0x7fffffff;
		if (hgfs_channels.size() >= 32 ||
		    (type != 0x49435052 && type != 0x4f4c4354)) {
			reg_ecx = 0;
			return;
		}
		while (!hgfs_next_channel || hgfs_channels.count(hgfs_next_channel)) {
			++hgfs_next_channel;
		}
		const auto id = hgfs_next_channel++;
		auto& c       = hgfs_channels[id];
		c.type        = type;
		c.id          = id;
		reg_edx       = uint32_t(id) << 16;
		reg_esi       = 0x13572468;
		reg_edi       = 0x24681357;
		reg_ecx       = 0x10000;
		return;
	}
	const auto id = uint16_t(reg_edx >> 16);
	auto it       = hgfs_channels.find(id);
	if (it == hgfs_channels.end()) {
		reg_ecx = 0;
		return;
	}
	if (reg_esi != 0x13572468 || reg_edi != 0x24681357) {
		reg_ecx = 0;
		return;
	}
	auto& c = it->second;
	reg_ecx = 0x10000;
	switch (command) {
	case 1:
		if (reg_ebx > 61440) {
			c.expected = 0;
			c.request.clear();
			c.reply.clear();
			reg_ecx = 0;
			break;
		}
		c.expected = reg_ebx;
		c.request.clear();
		c.reply.clear();
		c.cursor = 0;
		if (!c.expected) {
			hgfs_dispatch(c);
		}
		break;
	case 3:
		reg_ebx = uint32_t(c.reply.size());
		reg_edx = 0x10000;
		reg_ecx = c.reply.empty() ? 0x10000 : 0x30000;
		break;
	case 5:
		c.reply.clear();
		c.cursor = 0;
		break;
	case 6: {
		const bool was_rpci = c.type == 0x49435052;
		hgfs_channels.erase(it);
		if (was_rpci) {
			VmwareHgfs::CloseSession(id);
			if (hgfs_trace) {
				LOG_MSG("HGFS: RPCI session closed; file handles released");
			}
		}
		break;
	}
	default: reg_ecx = 0; break;
	}
}
static void hgfs_write_byte(const io_port_t, const io_val_t value, const io_width_t)
{
	auto it = hgfs_channels.find(uint16_t(reg_edx >> 16));
	if (reg_eax != VmWareMagic || it == hgfs_channels.end() ||
	    reg_ebp != 0x13572468 || reg_edi != 0x24681357) {
		reg_ebx = 0;
		return;
	}
	auto& c = it->second;
	if (c.request.size() >= c.expected) {
		reg_ebx = 0;
		return;
	}
	c.request.push_back(uint8_t(value));
	if (c.request.size() == c.expected) {
		hgfs_dispatch(c);
	}
}
static uint32_t hgfs_read_byte(const io_port_t, const io_width_t)
{
	auto it = hgfs_channels.find(uint16_t(reg_edx >> 16));
	if (reg_eax != VmWareMagic || it == hgfs_channels.end() ||
	    reg_esi != 0x13572468 || reg_ebp != 0x24681357) {
		reg_ebx = 0;
		return 0;
	}
	auto& c = it->second;
	if (c.cursor >= c.reply.size()) {
		reg_ebx = 0;
		return 0;
	}
	return c.reply[c.cursor++];
}

static uint32_t port_read_vmware(const io_port_t, const io_width_t)
{
	if (reg_eax != VmWareMagic) {
		return 0;
	}

	if (hgfs_enabled && reg_cx == 0x1e) {
		hgfs_command();
		return reg_eax;
	}

	reg_eax = VmWareAcknowledged;

	switch (static_cast<VmWareCommand>(reg_cx)) {
	case VmWareCommand::GetVersion:
		command_get_version();
		break;
	case VmWareCommand::AbsPointerData:
		command_abs_pointer_data();
		break;
	case VmWareCommand::AbsPointerStatus:
		command_abs_pointer_status();
		break;
	case VmWareCommand::AbsPointerCommand:
		command_abs_pointer();
		break;
	case VmWareCommand::AbsPointerRestrict:
		LOG_WARNING("VMWARE: Mouse pointer restrictions not implemented");
		reg_eax = VmWareUnimplemented;
		break;
	default:
		LOG_WARNING("VMWARE: Unimplemented command 0x%08x", reg_ecx);
		reg_eax = VmWareUnimplemented;
		break;
	}

	return reg_eax;
}

// ***************************************************************************
// Intel 8042 interface
// ***************************************************************************

bool VMWARE_I8042_ReadTakeover()
{
	return is_i8042_unlocked;
}

uint32_t VMWARE_I8042_ReadStatusRegister()
{
	// Port 0x64 read handler
	assert(is_i8042_unlocked);

	mouse_status_to_abs_pointer_queue();

	uint32_t result = check_cast<uint16_t>(abs_pointer_queue.size());
	if (abs_pointer_queue_content == QueueContent::CommandResponse) {
		// Wild guess - there seems to be no specification available,
		// but this matches the VMware Workstation Player behavior
		result |= VmWareAcknowledged << 16;
	}

	return result;
}

uint32_t VMWARE_I8042_ReadDataPort()
{
	// Port 0x60 read handler
	assert(is_i8042_unlocked);

	return fetch_from_abs_pointer_queue();
}

bool VMWARE_I8042_WriteCommandPort(const uint32_t value)
{
	// Port 0x64 write handler
	if (!has_feature_mouse) {
		return false;
	}

	const auto command = static_cast<VmMouseCommand>(value);

	if (!is_i8042_unlocked && command == VmMouseCommand::Enable) {
		is_i8042_unlocked = true;
		// For the Intel 8042 VMware port interface we need the PS/2
		// auxilary (mouse) interrupts to be triggered immediately,
		// without creating mouse data packets - these are not being
		// fetched by the official Windows 9x VMware mouse driver
		MOUSEVMM_EnableImmediateInterrupts(true);
	}

	if (!is_i8042_unlocked) {
		return false;
	}

	execute_command(command);

	if (command == VmMouseCommand::Enable) {
		// Required by the official Windows 9x VMware mouse driver
		abs_pointer_queue         = {VmMouseVersionId};
		abs_pointer_queue_content = QueueContent::CommandResponse;

	} else if (command == VmMouseCommand::Disable) {
		is_i8042_unlocked = false;
		MOUSEVMM_EnableImmediateInterrupts(false);
	}

	return true;
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void VMWARE_NotifyBooting()
{
	hgfs_channels.clear();
	hgfs_next_channel = 1;
	VmwareHgfs::Reset();
	program_segment_name = {};
}

void VMWARE_NotifyProgramName(const std::string& segment_name)
{
	program_segment_name = segment_name;
	upcase(program_segment_name);
}

// ***************************************************************************
// Lifecycle
// ***************************************************************************

void VMWARE_Init()
{
	has_feature_mouse = MOUSEVMM_IsSupported(MouseVmmProtocol::VmWare);

	// TODO: implement more features:
	// - extend HGFS compatibility beyond VMSMOUNT:
	//   https://github.com/eduardocasino/vmsmount
	// - everything supported by the official Windows 9x VMware Tools
	// - (very far future) possibly Windows 9x 3D acceleration using
	//   project like SoftGPU (or whatever will be available):
	//   https://github.com/JHRobotics/softgpu

    const auto section = get_section("hgfs");
    const auto root = section->GetPath("root")->realpath.string();
    const auto allow_write = section->GetBool("write");
    hgfs_trace = section->GetBool("trace");
    hgfs_enabled = VmwareHgfs::Init(root.c_str(), allow_write, section->GetBool("flat"));
    if (!root.empty() && !hgfs_enabled) {
        LOG_WARNING("HGFS: Could not open shared directory '%s'", root.c_str());
    }
    for (int i = 1; i <= 8; ++i) {
        const auto prefix = "share" + std::to_string(i);
        const auto name = std::string(section->GetString((prefix + "_name").c_str()));
        const auto path = section->GetPath((prefix + "_root").c_str())->realpath.string();
        if (name.empty() && path.empty()) { continue; }
        if (VmwareHgfs::AddShare(name.c_str(), path.c_str(),
                                section->GetBool((prefix + "_write").c_str()))) {
            hgfs_enabled = true;
            LOG_MSG("HGFS: named share %s enabled", name.c_str());
        } else {
            LOG_WARNING("HGFS: cannot enable named share %s", name.c_str());
        }
    }
	if (hgfs_enabled) {
		IO_RegisterReadHandler(0x5659, hgfs_read_byte, io_width_t::byte);
		IO_RegisterWriteHandler(0x5659, hgfs_write_byte, io_width_t::byte);
		LOG_MSG("HGFS: SHARED folder enabled (%s)", allow_write ? "read/write" : "read-only");
	}
	is_interface_enabled = has_feature_mouse || hgfs_enabled;
	if (is_interface_enabled) {
		IO_RegisterReadHandler(port_num_vmware,
		                       port_read_vmware,
		                       io_width_t::dword);
	}
}

void VMWARE_Destroy()
{
	if (hgfs_enabled) {
		IO_FreeReadHandler(0x5659, io_width_t::byte);
		IO_FreeWriteHandler(0x5659, io_width_t::byte);
	}
	hgfs_channels.clear();
	hgfs_next_channel = 1;
	hgfs_enabled      = false;
	VmwareHgfs::Init(nullptr);
	if (is_interface_enabled) {
		IO_FreeReadHandler(port_num_vmware, io_width_t::dword);
		is_interface_enabled = false;
	}
}

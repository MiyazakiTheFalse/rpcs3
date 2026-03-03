#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/system_config.h"

#include "Emu/Cell/ErrorCodes.h"
#include "sys_event.h"
#include "sys_fs.h"

#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include "util/shared_ptr.hpp"

#include "sys_storage.h"

LOG_CHANNEL(sys_storage);

namespace
{
	struct storage_manager
	{
		// This is probably wrong and should be assigned per fd or something
		atomic_ptr<lv2_event_queue> asyncequeue;

		std::mutex async_mutex;
		std::condition_variable async_cv;
		u32 in_flight = 0;
		u64 next_token = 1;
		std::unordered_set<u64> cancelled_tokens;

		bool is_shaping_enabled() const
		{
			return g_cfg.vfs.emulate_ps3_hdd_mode;
		}

		u32 queue_depth() const
		{
			return std::max<u32>(1, sys_fs_get_hdd_shape_queue_depth());
		}
	};

	enum class storage_async_op : u64
	{
		read = 1,
		write = 2,
		cancel = 3,
	};

	class storage_async_slot
	{
	public:
		storage_async_slot(storage_manager& manager, bool enabled)
			: m_manager(manager)
			, m_enabled(enabled)
		{
			if (!m_enabled)
			{
				return;
			}

			std::unique_lock lock(m_manager.async_mutex);
			m_manager.async_cv.wait(lock, [&]
			{
				return m_manager.in_flight < m_manager.queue_depth();
			});
			m_manager.in_flight++;
		}

		~storage_async_slot()
		{
			if (!m_enabled)
			{
				return;
			}

			std::lock_guard lock(m_manager.async_mutex);
			m_manager.in_flight--;
			m_manager.async_cv.notify_one();
		}

	private:
		storage_manager& m_manager;
		bool m_enabled = false;
	};

	void send_storage_async_event(storage_manager& manager, u64 token, storage_async_op op, error_code result)
	{
		if (auto q = manager.asyncequeue.load())
		{
			q->send(0, token, static_cast<u64>(op), static_cast<u64>(result));
		}
	}

	u64 allocate_storage_token(storage_manager& manager)
	{
		std::lock_guard lock(manager.async_mutex);
		return manager.next_token++;
	}

	bool is_storage_token_cancelled(storage_manager& manager, u64 token)
	{
		std::lock_guard lock(manager.async_mutex);
		if (const auto it = manager.cancelled_tokens.find(token); it != manager.cancelled_tokens.end())
		{
			manager.cancelled_tokens.erase(it);
			return true;
		}
		return false;
	}
}

error_code sys_storage_open(u64 device, u64 mode, vm::ptr<u32> fd, u64 flags)
{
	sys_storage.todo("sys_storage_open(device=0x%x, mode=0x%x, fd=*0x%x, flags=0x%x)", device, mode, fd, flags);

	if (device == 0)
	{
		return CELL_ENOENT;
	}

	if (!fd)
	{
		return CELL_EFAULT;
	}

	[[maybe_unused]] u64 storage_id = device & 0xFFFFF00FFFFFFFF;
	fs::file file;

	if (const u32 id = idm::make<lv2_storage>(device, std::move(file), mode, flags))
	{
		*fd = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_storage_close(u32 fd)
{
	sys_storage.todo("sys_storage_close(fd=0x%x)", fd);

	idm::remove<lv2_storage>(fd);

	return CELL_OK;
}

error_code sys_storage_read(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> bounce_buf, vm::ptr<u32> sectors_read, u64 flags)
{
	sys_storage.todo("sys_storage_read(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, bounce_buf=*0x%x, sectors_read=*0x%x, flags=0x%x)", fd, mode, start_sector, num_sectors, bounce_buf, sectors_read, flags);

	if (!bounce_buf || !sectors_read)
	{
		return CELL_EFAULT;
	}

	std::memset(bounce_buf.get_ptr(), 0, num_sectors * 0x200ull);
	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	if (handle->file)
	{
		handle->file.seek(start_sector * 0x200ull);
		const u64 size = num_sectors * 0x200ull;
		const u64 result = lv2_file::op_read(handle->file, bounce_buf, size);
		num_sectors = ::narrow<u32>(result / 0x200ull);
	}

	*sectors_read = num_sectors;

	return CELL_OK;
}

error_code sys_storage_write(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> data, vm::ptr<u32> sectors_wrote, u64 flags)
{
	sys_storage.todo("sys_storage_write(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, data=*=0x%x, sectors_wrote=*0x%x, flags=0x%llx)", fd, mode, start_sector, num_sectors, data, sectors_wrote, flags);

	if (!sectors_wrote)
	{
		return CELL_EFAULT;
	}

	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	*sectors_wrote = num_sectors;

	return CELL_OK;
}

error_code sys_storage_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen)
{
	sys_storage.todo("sys_storage_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%, inlen=0x%x, out=*0x%x, outlen=0x%x)", dev_handle, cmd, in, inlen, out, outlen);

	return CELL_OK;
}

error_code sys_storage_async_configure(u32 fd, u32 io_buf, u32 equeue_id, u32 unk)
{
	sys_storage.todo("sys_storage_async_configure(fd=0x%x, io_buf=0x%x, equeue_id=0x%x, unk=*0x%x)", fd, io_buf, equeue_id, unk);

	auto& manager = g_fxo->get<storage_manager>();

	if (auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id))
	{
		manager.asyncequeue.store(queue);
	}
	else
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_storage_async_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen, u64 unk)
{
	sys_storage.todo("sys_storage_async_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%x, inlen=0x%x, out=*0x%x, outlen=0x%x, unk=0x%x)", dev_handle, cmd, in, inlen, out, outlen, unk);

	auto& manager = g_fxo->get<storage_manager>();

	if (auto q = manager.asyncequeue.load())
	{
		q->send(0, unk, unk, unk);
	}

	return CELL_OK;
}

error_code sys_storage_async_read(ppu_thread& ppu)
{
	auto& manager = g_fxo->get<storage_manager>();

	const u32 fd = static_cast<u32>(ppu.gpr[3]);
	const u32 mode = static_cast<u32>(ppu.gpr[4]);
	const u32 start_sector = static_cast<u32>(ppu.gpr[5]);
	const u32 num_sectors = static_cast<u32>(ppu.gpr[6]);
	const vm::ptr<void> bounce_buf = vm::ptr<void>::make(static_cast<u32>(ppu.gpr[7]));
	const vm::ptr<u32> sectors_read = vm::ptr<u32>::make(static_cast<u32>(ppu.gpr[8]));
	const u64 flags = ppu.gpr[9];

	const u64 token = allocate_storage_token(manager);
	storage_async_slot slot(manager, manager.is_shaping_enabled());

	error_code result = CELL_ECANCELED;
	if (!is_storage_token_cancelled(manager, token))
	{
		result = sys_storage_read(fd, mode, start_sector, num_sectors, bounce_buf, sectors_read, flags);
	}

	send_storage_async_event(manager, token, storage_async_op::read, result);
	return result;
}

error_code sys_storage_async_write(ppu_thread& ppu)
{
	auto& manager = g_fxo->get<storage_manager>();

	const u32 fd = static_cast<u32>(ppu.gpr[3]);
	const u32 mode = static_cast<u32>(ppu.gpr[4]);
	const u32 start_sector = static_cast<u32>(ppu.gpr[5]);
	const u32 num_sectors = static_cast<u32>(ppu.gpr[6]);
	const vm::ptr<void> data = vm::ptr<void>::make(static_cast<u32>(ppu.gpr[7]));
	const vm::ptr<u32> sectors_wrote = vm::ptr<u32>::make(static_cast<u32>(ppu.gpr[8]));
	const u64 flags = ppu.gpr[9];

	const u64 token = allocate_storage_token(manager);
	storage_async_slot slot(manager, manager.is_shaping_enabled());

	error_code result = CELL_ECANCELED;
	if (!is_storage_token_cancelled(manager, token))
	{
		result = sys_storage_write(fd, mode, start_sector, num_sectors, data, sectors_wrote, flags);
	}

	send_storage_async_event(manager, token, storage_async_op::write, result);
	return result;
}

error_code sys_storage_async_cancel(ppu_thread& ppu)
{
	auto& manager = g_fxo->get<storage_manager>();
	const u64 token = ppu.gpr[3];

	{
		std::lock_guard lock(manager.async_mutex);
		manager.cancelled_tokens.emplace(token);
	}

	send_storage_async_event(manager, token, storage_async_op::cancel, CELL_ECANCELED);
	return CELL_OK;
}

error_code sys_storage_get_device_info(u64 device, vm::ptr<StorageDeviceInfo> buffer)
{
	sys_storage.todo("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);

	if (!buffer)
	{
		return CELL_EFAULT;
	}

	memset(buffer.get_ptr(), 0, sizeof(StorageDeviceInfo));

	u64 storage = device & 0xFFFFF00FFFFFFFF;
	u32 dev_num = (device >> 32) & 0xFF;

	if (storage == ATA_HDD) // dev_hdd?
	{
		if (dev_num > 2)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 1;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// set partition size based on dev_num
		// stole these sizes from kernel dump, unknown if they are 100% correct
		// vsh reports only 2 partitions even though there is 3 sizes
		switch (dev_num)
		{
		case 0:
			buffer->sector_count = 0x2542EAB0; // possibly total size
			break;
		case 1:
			buffer->sector_count = 0x24FAEA98; // which makes this hdd0
			break;
		case 2:
			buffer->sector_count = 0x3FFFF8; // and this one hdd1
			break;
		}
	}
	else if (storage == BDVD_DRIVE) //	dev_bdvd?
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_count = 0x4D955;
		buffer->sector_size = 0x800;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;
	}
	else if (storage == USB_MASS_STORAGE_1(0))
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		/*buffer->sector_count = 0x4D955;*/
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;
	}
	else if (storage == NAND_FLASH)
	{
		if (dev_num > 6)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 1;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x80000;
			break;
		case 1: buffer->sector_count = 0x75F8;
			break;
		case 2: buffer->sector_count = 0x63E00;
			break;
		case 3: buffer->sector_count = 0x8000;
			break;
		case 4: buffer->sector_count = 0x400;
			break;
		case 5: buffer->sector_count = 0x2000;
			break;
		case 6: buffer->sector_count = 0x200;
			break;
		}
	}
	else if (storage == NOR_FLASH)
	{
		if (dev_num > 3)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x8000;
			break;
		case 1: buffer->sector_count = 0x77F8;
			break;
		case 2: buffer->sector_count = 0x100; // offset, 0x20000
			break;
		case 3: buffer->sector_count = 0x400;
			break;
		}
	}
	else if (storage == NAND_UNK)
	{
		if (dev_num > 1)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x800;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x7FFFFFFF;
			break;
		}
	}
	else
	{
		sys_storage.error("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);
	}

	return CELL_OK;
}

error_code sys_storage_get_device_config(vm::ptr<u32> storages, vm::ptr<u32> devices)
{
	sys_storage.todo("sys_storage_get_device_config(storages=*0x%x, devices=*0x%x)", storages, devices);

	if (storages) *storages = 6; else return CELL_EFAULT;
	if (devices)  *devices = 17; else return CELL_EFAULT;

	return CELL_OK;
}

error_code sys_storage_report_devices(u32 storages, u32 start, u32 devices, vm::ptr<u64> device_ids)
{
	sys_storage.todo("sys_storage_report_devices(storages=0x%x, start=0x%x, devices=0x%x, device_ids=0x%x)", storages, start, devices, device_ids);

	if (!device_ids)
	{
		return CELL_EFAULT;
	}

	static constexpr std::array<u64, 0x11> all_devs = []
	{
		std::array<u64, 0x11> all_devs{};
		all_devs[0] = 0x10300000000000A;

		for (int i = 0; i < 7; ++i)
		{
			all_devs[i + 1] = 0x100000000000001 | (static_cast<u64>(i) << 32);
		}

		for (int i = 0; i < 3; ++i)
		{
			all_devs[i + 8] = 0x101000000000007 | (static_cast<u64>(i) << 32);
		}

		all_devs[11] = 0x101000000000006;

		for (int i = 0; i < 4; ++i)
		{
			all_devs[i + 12] = 0x100000000000004 | (static_cast<u64>(i) << 32);
		}

		all_devs[16] = 0x100000000000003;
		return all_devs;
	}();

	if (!devices || start >= all_devs.size() || devices > all_devs.size() - start)
	{
		return CELL_EINVAL;
	}

	std::copy_n(all_devs.begin() + start, devices, device_ids.get_ptr());

	return CELL_OK;
}

error_code sys_storage_configure_medium_event(u32 fd, u32 equeue_id, u32 c)
{
	sys_storage.todo("sys_storage_configure_medium_event(fd=0x%x, equeue_id=0x%x, c=0x%x)", fd, equeue_id, c);

	return CELL_OK;
}

error_code sys_storage_set_medium_polling_interval()
{
	sys_storage.todo("sys_storage_set_medium_polling_interval()");

	return CELL_OK;
}

error_code sys_storage_create_region()
{
	sys_storage.todo("sys_storage_create_region()");

	return CELL_OK;
}

error_code sys_storage_delete_region()
{
	sys_storage.todo("sys_storage_delete_region()");

	return CELL_OK;
}

error_code sys_storage_execute_device_command(u32 fd, u64 cmd, vm::ptr<char> cmdbuf, u64 cmdbuf_size, vm::ptr<char> databuf, u64 databuf_size, vm::ptr<u32> driver_status)
{
	sys_storage.todo("sys_storage_execute_device_command(fd=0x%x, cmd=0x%llx, cmdbuf=*0x%x, cmdbuf_size=0x%llx, databuf=*0x%x, databuf_size=0x%llx, driver_status=*0x%x)", fd, cmd, cmdbuf, cmdbuf_size, databuf, databuf_size, driver_status);

	// cmd == 2 is get device info,
	// databuf, first byte 0 == status ok?
	// byte 1, if < 0 , not ata device
	return CELL_OK;
}

error_code sys_storage_check_region_acl()
{
	sys_storage.todo("sys_storage_check_region_acl()");

	return CELL_OK;
}

error_code sys_storage_set_region_acl()
{
	sys_storage.todo("sys_storage_set_region_acl()");

	return CELL_OK;
}

error_code sys_storage_get_region_offset()
{
	sys_storage.todo("sys_storage_get_region_offset()");

	return CELL_OK;
}

error_code sys_storage_set_emulated_speed(ppu_thread& ppu)
{
	const u32 profile = static_cast<u32>(ppu.gpr[3]);

	// Keep this lightweight: map profile id to existing runtime HDD shaping knobs.
	switch (profile)
	{
	case 0:
		g_cfg.vfs.emulate_ps3_hdd_mode.set(false);
		break;
	case 1: // conservative
		g_cfg.vfs.emulate_ps3_hdd_mode.set(true);
		g_cfg.vfs.hdd_base_latency_us.set(3000);
		g_cfg.vfs.hdd_random_extra_latency_us.set(3000);
		g_cfg.vfs.hdd_read_mb_s.set(32);
		g_cfg.vfs.hdd_write_mb_s.set(16);
		g_cfg.vfs.hdd_queue_depth.set(2);
		break;
	case 2: // default PS3-like
		g_cfg.vfs.emulate_ps3_hdd_mode.set(true);
		g_cfg.vfs.hdd_base_latency_us.set(1500);
		g_cfg.vfs.hdd_random_extra_latency_us.set(2000);
		g_cfg.vfs.hdd_read_mb_s.set(48);
		g_cfg.vfs.hdd_write_mb_s.set(24);
		g_cfg.vfs.hdd_queue_depth.set(4);
		break;
	default: // fast profile
		g_cfg.vfs.emulate_ps3_hdd_mode.set(true);
		g_cfg.vfs.hdd_base_latency_us.set(250);
		g_cfg.vfs.hdd_random_extra_latency_us.set(500);
		g_cfg.vfs.hdd_read_mb_s.set(100);
		g_cfg.vfs.hdd_write_mb_s.set(60);
		g_cfg.vfs.hdd_queue_depth.set(8);
		break;
	}

	sys_fs_refresh_hdd_shape_profile();
	return CELL_OK;
}

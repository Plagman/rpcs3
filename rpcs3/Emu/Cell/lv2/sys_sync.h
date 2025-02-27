﻿#pragma once

#include "Utilities/mutex.h"
#include "Utilities/sema.h"
#include "Utilities/cond.h"

#include "Emu/Memory/vm_locking.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/IdManager.h"
#include "Emu/IPC.h"
#include "Emu/System.h"

#include <deque>
#include <thread>

// attr_protocol (waiting scheduling policy)
enum
{
	SYS_SYNC_FIFO                = 0x1, // First In, First Out Order
	SYS_SYNC_PRIORITY            = 0x2, // Priority Order
	SYS_SYNC_PRIORITY_INHERIT    = 0x3, // Basic Priority Inheritance Protocol
	SYS_SYNC_RETRY               = 0x4, // Not selected while unlocking
	SYS_SYNC_ATTR_PROTOCOL_MASK  = 0xf,
};

// attr_recursive (recursive locks policy)
enum
{
	SYS_SYNC_RECURSIVE           = 0x10,
	SYS_SYNC_NOT_RECURSIVE       = 0x20,
	SYS_SYNC_ATTR_RECURSIVE_MASK = 0xf0,
};

// attr_pshared (sharing among processes policy)
enum
{
	SYS_SYNC_PROCESS_SHARED      = 0x100,
	SYS_SYNC_NOT_PROCESS_SHARED  = 0x200,
	SYS_SYNC_ATTR_PSHARED_MASK   = 0xf00,
};

// attr_flags (creation policy)
enum
{
	SYS_SYNC_NEWLY_CREATED       = 0x1, // Create new object, fails if specified IPC key exists
	SYS_SYNC_NOT_CREATE          = 0x2, // Reference existing object, fails if IPC key not found
	SYS_SYNC_NOT_CARE            = 0x3, // Reference existing object, create new one if IPC key not found
	SYS_SYNC_ATTR_FLAGS_MASK     = 0xf,
};

// attr_adaptive
enum
{
	SYS_SYNC_ADAPTIVE            = 0x1000,
	SYS_SYNC_NOT_ADAPTIVE        = 0x2000,
	SYS_SYNC_ATTR_ADAPTIVE_MASK  = 0xf000,
};

// Base class for some kernel objects (shared set of 8192 objects).
struct lv2_obj
{
	using id_type = lv2_obj;

	static const u32 id_step = 0x100;
	static const u32 id_count = 8192;

	// Find and remove the object from the container (deque or vector)
	template <typename T, typename E>
	static bool unqueue(std::deque<T*>& queue, const E& object)
	{
		for (auto found = queue.cbegin(), end = queue.cend(); found != end; found++)
		{
			if (*found == object)
			{
				queue.erase(found);
				return true;
			}
		}

		return false;
	}

	template <typename E, typename T>
	static T* schedule(std::deque<T*>& queue, u32 protocol)
	{
		if (queue.empty())
		{
			return nullptr;
		}

		if (protocol == SYS_SYNC_FIFO)
		{
			const auto res = queue.front();
			queue.pop_front();
			return res;
		}

		u32 prio = -1;
		auto it = queue.cbegin();

		for (auto found = it, end = queue.cend(); found != end; found++)
		{
			const u32 _prio = static_cast<E*>(*found)->prio;

			if (_prio < prio)
			{
				it = found;
				prio = _prio;
			}
		}

		const auto res = *it;
		queue.erase(it);
		return res;
	}

	// Remove the current thread from the scheduling queue, register timeout
	static void sleep_timeout(cpu_thread&, u64 timeout);

	static void sleep(cpu_thread& thread, u64 timeout = 0)
	{
		vm::temporary_unlock(thread);
		sleep_timeout(thread, timeout);
	}

	static void yield(cpu_thread& thread)
	{
		vm::temporary_unlock(thread);
		awake(thread, -4);
	}

	// Schedule the thread
	static void awake(cpu_thread&, u32 prio);

	static void awake(cpu_thread& thread)
	{
		awake(thread, -1);
	}

	static void cleanup();

	template <typename T, typename F>
	static error_code create(u32 pshared, u64 ipc_key, s32 flags, F&& make)
	{
		switch (pshared)
		{
		case SYS_SYNC_PROCESS_SHARED:
		{
			switch (flags)
			{
			case SYS_SYNC_NEWLY_CREATED:
			case SYS_SYNC_NOT_CARE:
			{
				std::shared_ptr<T> result = make();

				if (!ipc_manager<T, u64>::add(ipc_key, [&] { if (!idm::import_existing<lv2_obj, T>(result)) result.reset(); return result; }, &result))
				{
					if (flags == SYS_SYNC_NEWLY_CREATED)
					{
						return CELL_EEXIST;
					}

					if (!idm::import_existing<lv2_obj, T>(result))
					{
						return CELL_EAGAIN;
					}

					return CELL_OK;
				}
				else if (!result)
				{
					return CELL_EAGAIN;
				}
				else
				{
					return CELL_OK;
				}
			}
			case SYS_SYNC_NOT_CREATE:
			{
				auto result = ipc_manager<T, u64>::get(ipc_key);

				if (!result)
				{
					return CELL_ESRCH;
				}

				if (!idm::import_existing<lv2_obj, T>(result))
				{
					return CELL_EAGAIN;
				}

				return CELL_OK;
			}
			default:
			{
				return CELL_EINVAL;
			}
			}
		}
		case SYS_SYNC_NOT_PROCESS_SHARED:
		{
			if (!idm::import<lv2_obj, T>(std::forward<F>(make)))
			{
				return CELL_EAGAIN;
			}

			return CELL_OK;
		}
		default:
		{
			return CELL_EINVAL;
		}
		}
	}

	template<bool is_usleep = false>
	static bool wait_timeout(u64 usec, cpu_thread* const cpu = nullptr)
	{
		static_assert(UINT64_MAX / cond_variable::max_timeout >= g_cfg.core.clocks_scale.max, "timeout may overflow during scaling");

		// Clamp to max timeout accepted
		if (usec > cond_variable::max_timeout) usec = cond_variable::max_timeout;

		// Now scale the result
		usec = (usec * g_cfg.core.clocks_scale) / 100;

#ifdef __linux__
		// TODO: Confirm whether Apple or any BSD can benefit from this as well
		constexpr u32 host_min_quantum = 50;
#else
		// Host scheduler quantum for windows (worst case)
		// NOTE: On ps3 this function has very high accuracy
		constexpr u32 host_min_quantum = 500;
#endif
		extern u64 get_system_time();

		u64 passed = 0;
		u64 remaining;

		const u64 start_time = get_system_time();
		while (usec >= passed)
		{
			remaining = usec - passed;

			if (g_cfg.core.sleep_timers_accuracy < (is_usleep ? sleep_timers_accuracy_level::_usleep : sleep_timers_accuracy_level::_all_timers))
			{
				thread_ctrl::wait_for(remaining);
			}
			else
			{
				if (remaining > host_min_quantum)
				{
#ifdef __linux__
					// Do not wait for the last quantum to avoid loss of accuracy
					thread_ctrl::wait_for(remaining - ((remaining % host_min_quantum) + host_min_quantum));
#else
					// Wait on multiple of min quantum for large durations to avoid overloading low thread cpus 
					thread_ctrl::wait_for(remaining - (remaining % host_min_quantum));
#endif
				}
				else
				{
					// Try yielding. May cause long wake latency but helps weaker CPUs a lot by alleviating resource pressure
					std::this_thread::yield();
				}
			}

			if (Emu.IsStopped())
			{
				return false;
			}

			if (cpu && cpu->state & cpu_flag::signal)
			{
				return false;
			}

			passed = get_system_time() - start_time;
		}

		return true;
	}

private:
	// Scheduler mutex
	static shared_mutex g_mutex;

	// Scheduler queue for active PPU threads
	static std::deque<class ppu_thread*> g_ppu;

	// Waiting for the response from
	static std::deque<class cpu_thread*> g_pending;

	// Scheduler queue for timeouts (wait until -> thread)
	static std::deque<std::pair<u64, class cpu_thread*>> g_waiting;

	static void schedule_all();
};

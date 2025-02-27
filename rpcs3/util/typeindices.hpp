﻿#pragma once

namespace stx
{
	template <typename Info>
	class type_counter;

	// Type information for given Info type, also serving as tag
	template <typename Info>
	class type_info final : public Info
	{
		// Current type id (non-zero)
		unsigned type = 0;

		// Next typeinfo in linked list
		type_info* next = nullptr;

		type_info(Info info, decltype(sizeof(int))) noexcept;

		friend type_counter<Info>;

	public:
		constexpr type_info() noexcept = default;

		unsigned index() const
		{
			return type;
		}
	};

	// Class for automatic type registration for given Info type
	template <typename Info>
	class type_counter final
	{
		// Dummy first element to simplify list filling logic
		type_info<Info> first{};

		// Linked list built at global initialization time
		type_info<Info>* next = &first;

		friend type_info<Info>;

	public:
		constexpr type_counter() noexcept = default;

		unsigned count() const
		{
			return next->index();
		}

		class const_iterator
		{
			const type_info<Info>* ptr;

		public:
			const_iterator(const type_info<Info>* ptr)
				: ptr(ptr)
			{
			}

			const type_info<Info>& operator*() const
			{
				return *ptr;
			}

			const type_info<Info>* operator->() const
			{
				return ptr;
			}

			const_iterator& operator++()
			{
				ptr = ptr->next;
				return *this;
			}

			const_iterator operator++(int)
			{
				const_iterator r = ptr;
				ptr = ptr->next;
				return r;
			}

			bool operator==(const const_iterator& r) const
			{
				return ptr == r.ptr;
			}

			bool operator!=(const const_iterator& r) const
			{
				return ptr != r.ptr;
			}
		};

		const_iterator begin() const
		{
			return first.next;
		}

		const_iterator end() const
		{
			return nullptr;
		}

		// Global type info instance
		template <typename T>
		static inline const type_info<Info> type{Info::template make_typeinfo<T>(), sizeof(T)};
	};

	// Global typecounter instance
	template <typename Info>
	inline type_counter<Info> typeinfo_v{};

	template <typename Info>
	type_info<Info>::type_info(Info info, decltype(sizeof(int))) noexcept
		: Info(info)
		, type(typeinfo_v<Info>.count() + 1)
	{
		// Update linked list
		typeinfo_v<Info>.next->next = this;
		typeinfo_v<Info>.next       = this;
	}
}

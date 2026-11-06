#pragma once
// PDZ Restored (2026-11-03): smolv / zstd / xxhash / <execution> includes removed (only the Unleashed CLI used them).

#ifdef _WIN32
#include <Windows.h>
#endif

#include <dxcapi.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <cmath>
#include <fmt/core.h>
#include <string>
#include <unordered_map>

template<typename T>
static T byteSwap(T value)
{
    if constexpr (sizeof(T) == 1)
        return value;
    else if constexpr (sizeof(T) == 2)
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
    else if constexpr (sizeof(T) == 4)
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
    else if constexpr (sizeof(T) == 8) 
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));

    assert(false && "Unexpected byte size.");
    return value;
}

template<typename T>
struct be
{
    T value;

    T get() const
    {
        if constexpr (std::is_enum_v<T>)
            return T(byteSwap(std::underlying_type_t<T>(value)));
        else
            return byteSwap(value);
    }

    operator T() const
    {
        return get();
    }
};  

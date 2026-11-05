#pragma once

// Fixed-size integer names used throughout NVR (and in the engine headers).
using UInt8	= unsigned char;
using UInt16	= unsigned short;
using UInt32	= unsigned long;		// long, not int: matches the game's and xNVSE's headers
using UInt64	= unsigned long long;
using SInt8		= signed char;
using SInt16	= signed short;
using SInt32	= signed long;
using SInt64	= signed long long;
using Float32	= float;
using Float64	= double;

// ---------------------------------------------------------------------------------------------------------------------
// Calling game functions by address
// ---------------------------------------------------------------------------------------------------------------------

// __thiscall to the game function at Method on Instance; returns EAX. The address goes through a member function
// pointer of a throwaway class so the compiler emits a real thiscall (this in ECX, the rest on the stack).
template <typename... Args>
inline UInt32 ThisCall(UInt32 Method, void* Instance, Args... args) {
	class Any {};
	union { UInt32 address; UInt32 (Any::*method)(Args...); } call = { Method };
	return (reinterpret_cast<Any*>(Instance)->*call.method)(args...);
}

// The same for a game method returning float (ST0); ThisCallD hands that float back widened to double.
inline float ThisCallF(UInt32 Method, void* Instance) {
	class Any {};
	union { UInt32 address; float (Any::*method)(); } call = { Method };
	return (reinterpret_cast<Any*>(Instance)->*call.method)();
}
inline double ThisCallD(UInt32 Method, void* Instance) {
	return ThisCallF(Method, Instance);
}

template <typename Ret = void, typename... Args>
__forceinline Ret StdCall(UInt32 address, Args... args) {
	return reinterpret_cast<Ret (__stdcall*)(Args...)>(address)(std::forward<Args>(args)...);
}

template <typename Ret = UInt32, typename... Args>
__forceinline Ret ThisStdCall(UInt32 address, const void* instance, Args... args) {
	return reinterpret_cast<Ret (__thiscall*)(const void*, Args...)>(address)(instance, std::forward<Args>(args)...);
}

template <typename Ret = void, typename... Args>
__forceinline Ret CdeclCall(UInt32 address, Args... args) {
	return reinterpret_cast<Ret (__cdecl*)(Args...)>(address)(std::forward<Args>(args)...);
}

// ---------------------------------------------------------------------------------------------------------------------
// A flags word as the game stores it
// ---------------------------------------------------------------------------------------------------------------------

template <typename T> class Bitfield {
public:
	T field;

	Bitfield() : field(0) {}

	T Get() const { return field; }
	T GetBit(UInt32 bits) const { return field & bits; }					// the masked bits, not a bool
	void Set(UInt32 bits) { field |= bits; }
	void Clear(UInt32 bits) { field &= ~bits; }
	T GetField(T mask, T shift) const { return (field & mask) >> shift; }
	void SetField(T value, T mask, T shift) { field = (field & ~mask) | (value << shift); }
};

using Bitfield8	= Bitfield<UInt8>;
using Bitfield16	= Bitfield<UInt16>;
using Bitfield32	= Bitfield<UInt32>;

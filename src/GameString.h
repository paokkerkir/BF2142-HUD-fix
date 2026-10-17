#pragma once

// std::string as compiled into BF2142.exe (MSVCP71, 28 bytes). All work is
// delegated to the game's own MSVCP71 imports so ownership stays on its heap.
// IAT slots in BF2142.exe 1.51:
#define IAT_STRING_CTOR_CHAR   0x90D484 // basic_string(const char*)
#define IAT_STRING_CTOR_COPY   0x90D4BC // basic_string(const basic_string&)
#define IAT_STRING_DTOR        0x90D480 // ~basic_string()

namespace game {
	class string {
	public:
		string(const char* s) {
			(*(void(__thiscall**)(string*, const char*))IAT_STRING_CTOR_CHAR)(this, s);
		}
		// must be a real copy: by-value strings are destroyed by the callee,
		// a bitwise copy would be freed twice
		string(const string& o) {
			(*(void(__thiscall**)(string*, const string*))IAT_STRING_CTOR_COPY)(this, &o);
		}
		~string() {
			(*(void(__thiscall**)(string*))IAT_STRING_DTOR)(this);
		}
		string& operator=(const string&) = delete;

		const char* c_str() const { return 16 <= _Myres ? _Bx._Ptr : _Bx._Buf; }

	private:
		unsigned char _Alval[4];
		union { char _Buf[16]; char* _Ptr; } _Bx;
		unsigned int _Mysize;
		unsigned int _Myres;
	};
	static_assert(sizeof(string) == 0x1C, "MSVCP71 std::string is 28 bytes");
}

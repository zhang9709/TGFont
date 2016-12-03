#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <io.h>

#include "winmm.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/encodings.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/encodedstream.h"

using namespace rapidjson;

#pragma pack(push, 1)
struct jmp
{
	uint8_t opcode;
	size_t address;
};
#pragma pack(pop)

typedef HFONT(WINAPI* fnCreateFontIndirectW)(const LOGFONTW *lplf);
fnCreateFontIndirectW origAddr = nullptr;

struct font
{
	std::wstring replace;
	bool overrideSize;
	size_t size;
};

std::map<std::wstring, font> fontsMap;
bool debug = false;
FILE *logFile = nullptr;

__declspec(naked) HFONT WINAPI CallOrigFn(const LOGFONTW *lplf)
{
	_asm
	{
		mov edi, edi
		push ebp
		mov ebp, esp
		jmp origAddr
	}
}

bool Utf8ToUtf16(const char *source, GenericStringBuffer<UTF16<>> &target)
{
	bool success = true;
	GenericStringStream<UTF8<>> sourceStream(source);
	while (sourceStream.Peek() != '\0')
		if (!rapidjson::Transcoder<UTF8<>, UTF16<>>::Transcode(sourceStream, target))
		{
			success = true;
			break;
		}
	return success;
}

bool Utf16ToUtf8(const wchar_t *source, GenericStringBuffer<UTF8<>> &target)
{
	bool success = true;
	GenericStringStream<UTF16<>> sourceStream(source);
	while (sourceStream.Peek() != '\0')
		if (!rapidjson::Transcoder<UTF16<>, UTF8<>>::Transcode(sourceStream, target))
		{
			success = true;
			break;
		}
	return success;
}

HFONT WINAPI MyCreateFontIndirectW(LOGFONTW *lplf)
{
	if (debug && logFile)
	{
		GenericStringBuffer<UTF8<>> name;
		if (Utf16ToUtf8(lplf->lfFaceName, name))
		{
			fprintf_s(logFile, "[CreateFont] name = \"%s\", size = %d\r\n", name.GetString(), lplf->lfHeight);
			fflush(logFile);
		}
	}

	auto it = fontsMap.find(lplf->lfFaceName);
	if (it != fontsMap.end())
	{
		size_t len = it->second.replace.copy(lplf->lfFaceName, LF_FACESIZE);
		lplf->lfFaceName[len] = L'\0';

		if (it->second.overrideSize)
			lplf->lfHeight = it->second.size;
	}
	return CallOrigFn(lplf);
}

bool LoadSettings(HMODULE hModule, const wchar_t *fileName)
{
	bool ret = false;
	FILE *file;
	if (_wfopen_s(&file, fileName, L"rb") == 0)
	{
		do {
			char readBuffer[512];
			FileReadStream is(file, readBuffer, sizeof(readBuffer));
			EncodedInputStream<UTF8<>, FileReadStream> eis(is);

			GenericDocument<UTF16<>> dom;

			if (dom.ParseStream<0, UTF8<>>(eis).HasParseError() || !dom.IsObject())
				break;

			auto member = dom.FindMember(L"fonts");
			if (member != dom.MemberEnd() && member->value.IsObject())
			{
				for (auto it = member->value.MemberBegin(); it != member->value.MemberEnd(); ++it)
				{
					if (it->value.IsObject())
					{
						std::wstring find = std::wstring(it->name.GetString(), it->name.GetStringLength());

						auto replace = it->value.FindMember(L"replace");
						if (replace != it->value.MemberEnd() && replace->value.IsString())
						{
							auto size = it->value.FindMember(L"size");
							bool overrideSize = size != it->value.MemberEnd() && size->value.IsInt();
							size_t _size = overrideSize ? size->value.GetInt() : 0;

							std::wstring _replace = std::wstring(replace->value.GetString(), replace->value.GetStringLength());

							font fontInfo = { _replace, overrideSize, _size };
							fontsMap[find] = fontInfo;
						}
					}
				}
			}

			member = dom.FindMember(L"debug");
			if (member != dom.MemberEnd() && member->value.IsBool())
				debug = member->value.GetBool();

			ret = true;
		} while (0);
		fclose(file);
	}
	return ret;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hModule);

#if _DEBUG
		MessageBox(0, L"DLL_PROCESS_ATTACH", L"", 0);
#endif

		if (!LoadDLL())
			return FALSE;

		wchar_t path[MAX_PATH];
		if (GetModuleFileName(hModule, path, MAX_PATH))
		{
			auto c = wcsrchr(path, L'\\');
			if (c) c[1] = L'\0';
		}

		wchar_t jsonName[MAX_PATH];
		wcscpy_s(jsonName, path);
		wcscat_s(jsonName, L"TGFont.json");

		if (_waccess_s(jsonName, 0) != 0)
		{
			FILE *file;
			if (_wfopen_s(&file, jsonName, L"wb") == 0)
			{
				fputs("", file); // FIXME
				fclose(file);
			}
		}

		if (!LoadSettings(hModule, jsonName))
		{
			MessageBox(0, L"Error loading TGFont.json!", L"Error", MB_ICONERROR);
			return TRUE;
		}

		if (debug)
		{
			wchar_t logName[MAX_PATH];
			wcscpy_s(logName, path);
			wcscat_s(logName, L"TGFont.log");
			logFile = _wfsopen(logName, L"ab+", _SH_DENYWR);
		}

		size_t pfnCreateFontIndirectW = (size_t)GetProcAddress(GetModuleHandle(L"gdi32.dll"), "CreateFontIndirectW");
		if (pfnCreateFontIndirectW)
		{
			DWORD oldProtect;
			if (VirtualProtect((LPVOID)pfnCreateFontIndirectW, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				jmp *hook = (jmp *)pfnCreateFontIndirectW;
				hook->opcode = 0xE9; // jmp
				hook->address = (size_t)MyCreateFontIndirectW - (size_t)pfnCreateFontIndirectW - 5;
				origAddr = (fnCreateFontIndirectW)(pfnCreateFontIndirectW + 5);
				VirtualProtect((LPVOID)pfnCreateFontIndirectW, 5, oldProtect, &oldProtect);
			}
		}
	}
	else if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		if (logFile)
			fclose(logFile);
	}
	return TRUE;
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* explorer.exe 대역: 앱 폴더에 두면 그 앱의 "explorer /select,<경로>"(카톡 "폴더 열기")와 "explorer <폴더>" 를
   유닉스 파일 관리자(org.freedesktop.FileManager1)로 돌리고, 처리할 수 없는 인자는 prefix 의 원본 explorer.exe 로 넘긴다.
   ponytail: Wine 의 ShellExecute("explorer") 는 현재 폴더 → 시스템 경로 순으로 찾고 exe 폴더는 안 봄.
             앱의 현재 폴더가 설치 폴더일 때만 불리고, 아니면 원본이 뜸(기능만 안 됨) → 실행기에서 현재 폴더를 고정하면 됨 */
#include <windows.h>

/* ntdll export (Wine 전용 + CRT 대용) */
extern int __cdecl __wine_unix_spawnvp(char *const argv[], int wait);
extern char * CDECL wine_get_unix_file_name(LPCWSTR dos);
extern WCHAR * __cdecl wcschr(const WCHAR *, WCHAR);
extern WCHAR * __cdecl wcsrchr(const WCHAR *, WCHAR);
extern int __cdecl _wcsnicmp(const WCHAR *, const WCHAR *, size_t);

/* wine-desktop-integration 이 "우리 파일" 인지 판별하는 표지 */
__attribute__((used)) static const char marker[] = "wine-desktop-integration";

static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void release(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static char *utf8(const WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *r = alloc(n + 1);
    if (r) WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, NULL, NULL);
    return r;
}

/* 앞뒤 공백을 떼고 감싼 따옴표를 벗긴다 (s 를 고침) */
static WCHAR *unquote(WCHAR *s)
{
    WCHAR *e;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"')
    {
        if ((e = wcschr(++s, '"'))) *e = 0;  /* 닫는 따옴표 뒤는 무시 */
        return s;
    }
    for (e = s + lstrlenW(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    return s;
}

/* s 를 존재하는 가장 가까운 상위 폴더로 자른다 (s 를 고침). 없으면 NULL */
static WCHAR *nearest_folder(WCHAR *s)
{
    WCHAR *e;
    DWORD attr;
    while ((e = wcsrchr(s, '\\')))
    {
        if (e == s + 2 && s[1] == ':') { if (!e[1]) return NULL; e[1] = 0; }  /* "C:" 만 남기면 그 드라이브의 현재 폴더라 "C:\" */
        else *e = 0;
        if ((attr = GetFileAttributesW(s)) != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return s;
    }
    return NULL;
}

/* 헬퍼 show-items|show-folders <유닉스 경로> (URI 변환은 헬퍼가 함). 종료코드 0 이면 성공 */
static BOOL show(const char *cmd, const WCHAR *path)
{
    WCHAR helper[MAX_PATH];
    DWORD hl = GetEnvironmentVariableW(L"WINE_DESKTOP_INTEGRATION_HELPER", helper, MAX_PATH);
    char *argv[4];
    BOOL ok;
    argv[0] = utf8(hl && hl < MAX_PATH ? helper : L"wine-desktop-integration-helper");
    argv[1] = (char *)cmd;
    argv[2] = wine_get_unix_file_name(path);
    argv[3] = NULL;
    ok = argv[0] && argv[2] && __wine_unix_spawnvp(argv, 1) == 0;
    release(argv[0]);
    release(argv[2]);
    return ok;
}

/* prefix 의 원본 explorer.exe 를 원래 인자 그대로 실행. 성공하면 0 */
static int original(const WCHAR *args)
{
    WCHAR orig[MAX_PATH], self[MAX_PATH], *cmd;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    DWORD n = GetEnvironmentVariableW(L"WDI_EXPLORER_ORIGINAL", orig, MAX_PATH);  /* 시험용 스위치: 원본 대신 실행할 exe */

    if (!n || n >= MAX_PATH)
    {
        /* Wine 은 C:\windows 와 system32 양쪽에 둠. 존재하는 쪽 */
        n = GetWindowsDirectoryW(orig, MAX_PATH - 14);
        if (!n || n >= MAX_PATH - 14) return 1;
        lstrcatW(orig, L"\\explorer.exe");
        if (GetFileAttributesW(orig) == INVALID_FILE_ATTRIBUTES)
        {
            n = GetSystemDirectoryW(orig, MAX_PATH - 14);
            if (!n || n >= MAX_PATH - 14) return 1;
            lstrcatW(orig, L"\\explorer.exe");
        }
    }
    n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH || !lstrcmpiW(self, orig)) return 1;  /* 우리가 원본 자리에 놓인 경우: 재귀 방지 */

    if (!(cmd = alloc((lstrlenW(orig) + lstrlenW(args) + 4) * sizeof(WCHAR)))) return 1;
    lstrcpyW(cmd, L"\"");
    lstrcatW(cmd, orig);
    lstrcatW(cmd, L"\"");
    if (*args) { lstrcatW(cmd, L" "); lstrcatW(cmd, args); }
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ok = CreateProcessW(orig, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    release(cmd);
    if (!ok) return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

void WINAPI start(void)
{
    WCHAR *p = GetCommandLineW(), *args, *copy;
    char *u;
    BOOL q = FALSE;
    DWORD attr;

    /* argv[0] 건너뛰기 */
    for (; *p && (q || (*p != ' ' && *p != '\t')); p++) if (*p == '"') q = !q;
    while (*p == ' ' || *p == '\t') p++;
    args = p;

    /* ponytail: "/select,<경로>" 와 폴더 하나만 처리. /e, /root, /n 이나 "/select,..." 전체를 감싼 따옴표는 원본으로 넘김 */
    if ((copy = alloc((lstrlenW(args) + 1) * sizeof(WCHAR))))
    {
        lstrcpyW(copy, args);
        if (!_wcsnicmp(copy, L"/select,", 8))
        {
            p = unquote(copy + 8);
            if (*p && (u = wine_get_unix_file_name(p)))
            {
                release(u);
                if (show("show-items", p)) ExitProcess(0);
            }
            /* 부모 폴더가 없으면 변환이 실패함 → 있는 가장 가까운 상위 폴더를 엶 (Windows 탐색기와 비슷) */
            else if (*p && (p = nearest_folder(p)) && show("show-folders", p)) ExitProcess(0);
        }
        else
        {
            p = unquote(copy);
            if (*p && (attr = GetFileAttributesW(p)) != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
                show("show-folders", p))
                ExitProcess(0);
        }
    }
    ExitProcess(original(args));  /* 처리 못 함·헬퍼 실패 → 원본 */
}

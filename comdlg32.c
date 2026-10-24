/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* comdlg32 프록시: GetOpenFileNameW/GetSaveFileNameW 를 xdg-desktop-portal 헬퍼로 돌리고,
   처리할 수 없는 호출과 나머지 export 는 prefix 의 system32\comdlg32.dll(Wine 원본)로 넘긴다. */
#define _COMDLG32_  /* 우리가 export 하므로 dllimport 아님 */
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>

/* ntdll export (Wine 전용) */
extern int __cdecl __wine_unix_spawnvp(char *const argv[], int wait);
extern char * CDECL wine_get_unix_file_name(LPCWSTR dos);
extern WCHAR * CDECL wine_get_dos_file_name(LPCSTR unix);
extern WCHAR * __cdecl wcsrchr(const WCHAR *, WCHAR);

/* ponytail: 전역 하나라 스레드별 오류가 아님. 여러 스레드가 동시에 대화상자를 열면 섞일 수 있음 → TLS 로 바꾸면 됨 */
static BOOL g_ours;
static DWORD g_err;

typedef BOOL (WINAPI *ofn_fn)(OPENFILENAMEW *);

/* wine-desktop-integration 이 "우리 파일" 인지 판별하는 표지 */
__attribute__((used)) static const char marker[] = "wine-desktop-integration";

static HMODULE g_orig;  /* system32\comdlg32.dll (Wine 원본) */

static FARPROC wine_proc(const char *name) { return g_orig ? GetProcAddress(g_orig, name) : NULL; }

/* 원본을 못 불러오면 중계가 크래시 대신 실패값을 돌려주게 */
static UINT_PTR WINAPI miss_zero(void) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
static HRESULT WINAPI miss_hr(void) { SetLastError(ERROR_PROC_NOT_FOUND); return E_FAIL; }
static short WINAPI miss_neg(void) { SetLastError(ERROR_PROC_NOT_FOUND); return -1; }

/* 나머지 export: 인자 레지스터를 건드리지 않고 원본으로 jmp 하는 중계 (x86_64 전용) */
#define RELAYS \
    X(ChooseColorA, miss_zero) X(ChooseColorW, miss_zero) X(ChooseFontA, miss_zero) X(ChooseFontW, miss_zero) \
    X(DllGetClassObject, miss_hr) X(DllRegisterServer, miss_hr) X(DllUnregisterServer, miss_hr) \
    X(FindTextA, miss_zero) X(FindTextW, miss_zero) X(GetFileTitleA, miss_neg) X(GetFileTitleW, miss_neg) \
    X(GetOpenFileNameA, miss_zero) X(GetSaveFileNameA, miss_zero) X(LoadAlterBitmap, miss_zero) \
    X(PageSetupDlgA, miss_zero) X(PageSetupDlgW, miss_zero) X(PrintDlgA, miss_zero) \
    X(PrintDlgExA, miss_hr) X(PrintDlgExW, miss_hr) X(PrintDlgW, miss_zero) \
    X(ReplaceTextA, miss_zero) X(ReplaceTextW, miss_zero) X(WantArrows, miss_zero) \
    X(dwLBSubclass, miss_zero) X(dwOKSubclass, miss_zero)

#define X(n, d) __attribute__((used)) static void *p_##n; \
    __asm__(".text\n.globl " #n "\n" #n ":\n\tjmp *p_" #n "(%rip)\n");
RELAYS
#undef X

/* ponytail: 로더 락 안에서 LoadLibrary 함 (Windows 에선 금지, Wine 에선 동작).
   문제가 되면 첫 호출 때 해석하는 asm 스텁(인자 레지스터·xmm0-3 보존)으로 바꾸면 됨 */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, void *p)
{
    WCHAR path[MAX_PATH];
    UINT n;
    if (r != DLL_PROCESS_ATTACH) return TRUE;
    n = GetSystemDirectoryW(path, MAX_PATH - 14);
    if (n && n < MAX_PATH - 14)
    {
        lstrcatW(path, L"\\comdlg32.dll");
        g_orig = LoadLibraryW(path);
        if (g_orig == h) g_orig = NULL;  /* 우리가 system32 에 놓인 경우: 재귀 방지 */
    }
#define X(n, d) if (!(p_##n = wine_proc(#n))) p_##n = (void *)d;
    RELAYS
#undef X
    return TRUE;
}

static BOOL pass(const char *name, OPENFILENAMEW *ofn)
{
    ofn_fn f = (ofn_fn)wine_proc(name);
    g_ours = FALSE;
    if (!f) { g_ours = TRUE; g_err = CDERR_INITIALIZATION; return FALSE; }
    return f(ofn);
}

static BOOL done(BOOL ret, DWORD err) { g_ours = TRUE; g_err = err; return ret; }

static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void release(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* WCHAR 문자열(len<0 이면 NUL 까지) → UTF-8 */
static char *utf8(const WCHAR *s, int len)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    char *r = alloc(n + 1);
    if (r) WideCharToMultiByte(CP_UTF8, 0, s, len, r, n, NULL, NULL);
    return r;
}

static DWORD WINAPI spawn_thread(void *argv) { return (DWORD)__wine_unix_spawnvp(argv, 1); }

/* 헬퍼를 작업 스레드에서 기다리며 호출 스레드의 메시지는 계속 돌린다
   ponytail: 대기 중 앱이 같은 스레드에서 다시 대화상자를 부르면 겹쳐 뜨고 g_err 도 공유됨 → 재진입 플래그로 막으면 됨 */
static int run_helper(char **argv, HWND owner)
{
    DWORD code = (DWORD)-1;
    MSG msg;
    BOOL quit = FALSE, was_enabled = FALSE;
    WPARAM quit_code = 0;
    HANDLE th = CreateThread(NULL, 0, spawn_thread, argv, 0, NULL);
    if (!th) return -1;
    if (owner && (owner = GetAncestor(owner, GA_ROOT))) was_enabled = !EnableWindow(owner, FALSE);
    while (MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { quit = TRUE; quit_code = msg.wParam; continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    if (owner)
    {
        if (was_enabled) EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (quit) PostQuitMessage((int)quit_code);
    return (int)code;
}

static char *read_file(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf = NULL;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && (buf = alloc(size + 1)) && !ReadFile(f, buf, size, &got, NULL))
    {
        release(buf);
        buf = NULL;
    }
    CloseHandle(f);
    if (buf) buf[got] = 0;
    return buf;
}

#define RETRY ((DWORD)-1)  /* fill → portal_dialog: 대화상자를 다시 띄움 */

/* 결과(1행 필터 번호, 2행부터 유닉스 경로)를 OPENFILENAMEW 에 채운다 */
static BOOL fill(OPENFILENAMEW *ofn, char *res, BOOL save)
{
    WCHAR **paths, *name, *dot, *out;
    char *p, *end = res + lstrlenA(res);
    int n = 0, lines = 1, i, filter = 0, len, folder, flen;
    BOOL multi = (ofn->Flags & OFN_ALLOWMULTISELECT) != 0;
    DWORD need;
    BOOL ret = FALSE;

    /* ponytail: 줄 단위라 유닉스 경로에 개행이 들어 있으면 깨짐 → NUL 구분자로 바꾸면 됨 */
    for (p = res; p < end; p++) if (*p == '\n' || *p == '\r') { *p = 0; lines++; }
    for (p = res; *p >= '0' && *p <= '9'; p++) filter = filter * 10 + (*p - '0');
    if (!(paths = alloc(lines * sizeof(*paths)))) return done(FALSE, CDERR_MEMALLOCFAILURE);
    for (p = res + lstrlenA(res) + 1; p < end; p += lstrlenA(p) + 1)
    {
        if (!*p) continue;
        if (!(paths[n] = wine_get_dos_file_name(p))) { n = 0; break; }  /* 하나라도 실패하면 전체 넘김 */
        n++;
    }
    if (!n)
    {
        for (i = 0; paths[i]; i++) release(paths[i]);
        release(paths);
        return done(FALSE, CDERR_GENERALCODES);  /* 호출자에서 넘김 처리 */
    }

    /* 폴더 부분 길이(마지막 '\' 앞). 다중 선택인데 폴더가 서로 다르면 첫 파일만 단일 결과로 돌려줌 */
    name = wcsrchr(paths[0], '\\');
    folder = name ? (int)(name - paths[0]) : 0;
    for (i = 1; i < n; i++)
    {
        name = wcsrchr(paths[i], '\\');
        if (!name || name - paths[i] != folder || memcmp(paths[i], paths[0], folder * sizeof(WCHAR))) break;
    }
    if (i < n) n = 1;

    if (n == 1 || !multi)
    {
        const WCHAR *ext = ofn->lpstrDefExt;
        len = lstrlenW(paths[0]);
        name = paths[0] + (folder ? folder + 1 : 0);
        if (save && !wcsrchr(name, '.') && ext && *ext) len += 1 + lstrlenW(ext);
        else ext = NULL;
        need = len + 1 + multi;  /* 다중 선택 플래그면 끝 NUL 하나 더 (Wine 과 같음) */
        if (need > ofn->nMaxFile)
        {
            *(WORD *)ofn->lpstrFile = (WORD)need;
            ret = done(FALSE, FNERR_BUFFERTOOSMALL);
            goto out;
        }
        lstrcpyW(ofn->lpstrFile, paths[0]);
        if (ext) { lstrcatW(ofn->lpstrFile, L"."); lstrcatW(ofn->lpstrFile, ext); }
        if (multi) ofn->lpstrFile[len + 1] = 0;
        /* 포털은 사용자가 고른 이름만 덮어쓰기 확인을 하므로, 확장자를 덧붙인 경우는 여기서 묻는다 */
        if (ext && (ofn->Flags & OFN_OVERWRITEPROMPT) && GetFileAttributesW(ofn->lpstrFile) != INVALID_FILE_ATTRIBUTES)
        {
            WCHAR *msg = alloc((len + 64) * sizeof(WCHAR));
            int yes = 0;
            if (msg)
            {
                lstrcpyW(msg, ofn->lpstrFile + (name - paths[0]));
                lstrcatW(msg, L" already exists.\nDo you want to replace it?");
                yes = MessageBoxW(ofn->hwndOwner, msg, ofn->lpstrTitle ? ofn->lpstrTitle : L"Save As",
                                  MB_YESNO | MB_ICONEXCLAMATION) == IDYES;
                release(msg);
            }
            if (!yes) { ret = done(FALSE, RETRY); goto out; }
        }
        name = ofn->lpstrFile + (name - paths[0]);
        dot = wcsrchr(name, '.');
        ofn->nFileOffset = (WORD)(name - ofn->lpstrFile);
        ofn->nFileExtension = dot ? (WORD)(dot + 1 - ofn->lpstrFile) : 0;
    }
    else
    {
        flen = (folder == 2 && paths[0][1] == ':') ? 3 : folder;  /* 드라이브 루트는 "X:\" */
        need = flen + 2;  /* 폴더 + NUL + 끝 NUL */
        for (i = 0; i < n; i++) need += lstrlenW(paths[i] + folder + 1) + 1;
        if (need > ofn->nMaxFile)
        {
            *(WORD *)ofn->lpstrFile = (WORD)need;
            ret = done(FALSE, FNERR_BUFFERTOOSMALL);
            goto out;
        }
        out = ofn->lpstrFile;
        memcpy(out, paths[0], flen * sizeof(WCHAR));
        out += flen;
        *out++ = 0;
        for (i = 0; i < n; i++)
        {
            lstrcpyW(out, paths[i] + folder + 1);
            out += lstrlenW(out) + 1;
        }
        *out = 0;
        ofn->nFileOffset = (WORD)(flen + 1);
        ofn->nFileExtension = 0;
        name = ofn->lpstrFile + flen + 1;  /* 파일 제목은 첫 파일 */
    }
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) lstrcpynW(ofn->lpstrFileTitle, name, ofn->nMaxFileTitle);
    if (filter) ofn->nFilterIndex = filter;
    ret = done(TRUE, 0);
out:
    for (i = 0; paths[i]; i++) release(paths[i]);
    release(paths);
    return ret;
}

/* 동적 문자열은 own 에도 기록해 두고 끝에 한꺼번에 해제 */
static char *own(char **list, int *n, char *s) { return list[(*n)++] = s; }

/* "--key=" + 값 (값이 '-' 로 시작해도 안전). val 은 해제됨, NULL 이면 NULL */
static char *kv(const char *key, char *val)
{
    char *r = val ? alloc(lstrlenA(key) + lstrlenA(val) + 1) : NULL;
    if (r) { lstrcpyA(r, key); lstrcatA(r, val); }
    release(val);
    return r;
}

static BOOL portal_dialog(OPENFILENAMEW *ofn, BOOL save)
{
    const char *name = save ? "GetSaveFileNameW" : "GetOpenFileNameW";
    WCHAR tmpdir[MAX_PATH], tmp[MAX_PATH], helper[MAX_PATH], dir[MAX_PATH];
    char **argv, **dyn, *res = NULL, idx[32];
    const WCHAR *s, *file = NULL, *slash = NULL;
    int argc = 0, ndyn = 0, nstr = 0, i, code = -1;
    DWORD hl;
    BOOL ret = FALSE, fallback = TRUE;

    if ((ofn->lStructSize != sizeof(*ofn) && ofn->lStructSize != OPENFILENAME_SIZE_VERSION_400W) ||
        (ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) ||
        ((ofn->Flags & OFN_ALLOWMULTISELECT) && !(ofn->Flags & OFN_EXPLORER)) ||
        !ofn->lpstrFile || !ofn->nMaxFile)
        return pass(name, ofn);

    if (!GetTempPathW(MAX_PATH, tmpdir) || !GetTempFileNameW(tmpdir, L"wpf", 0, tmp)) return pass(name, ofn);

    for (s = ofn->lpstrFilter; s && *s; s += lstrlenW(s) + 1) nstr++;
    argv = alloc((16 + 2 * nstr) * sizeof(char *));
    dyn = alloc((16 + 2 * nstr) * sizeof(char *));
    if (!argv || !dyn) goto out;

    hl = GetEnvironmentVariableW(L"WINE_DESKTOP_INTEGRATION_HELPER", helper, MAX_PATH);
    if (!hl || hl >= MAX_PATH) hl = GetEnvironmentVariableW(L"WINE_PORTAL_FILEDIALOG_HELPER", helper, MAX_PATH);  /* 옛 이름 */
    argv[argc++] = own(dyn, &ndyn, utf8(hl && hl < MAX_PATH ? helper : L"wine-desktop-integration-helper", -1));
    argv[argc++] = save ? "--mode=save" : "--mode=open";
    argv[argc++] = own(dyn, &ndyn, kv("--out=", wine_get_unix_file_name(tmp)));
    if (ofn->lpstrTitle && *ofn->lpstrTitle) argv[argc++] = own(dyn, &ndyn, kv("--title=", utf8(ofn->lpstrTitle, -1)));
    /* lpstrFile 을 폴더/파일명으로 나눈다. 폴더가 있으면 lpstrInitialDir 보다 우선 (MS 문서) */
    if (*ofn->lpstrFile)
    {
        file = ofn->lpstrFile;
        slash = wcsrchr(file, '\\');
    }
    dir[0] = 0;
    if (slash && slash - file + 2 <= MAX_PATH)
        lstrcpynW(dir, file, (int)(slash - file) + 2);  /* 끝 '\' 포함 */
    else if (ofn->lpstrInitialDir && *ofn->lpstrInitialDir)
        lstrcpynW(dir, ofn->lpstrInitialDir, MAX_PATH);
    if (dir[0] && (argv[argc] = own(dyn, &ndyn, kv("--dir=", wine_get_unix_file_name(dir))))) argc++;
    if (save && file && *(slash ? slash + 1 : file))
        argv[argc++] = own(dyn, &ndyn, kv("--name=", utf8(slash ? slash + 1 : file, -1)));
    if (ofn->Flags & OFN_ALLOWMULTISELECT) argv[argc++] = "--multiple";
    for (s = ofn->lpstrFilter, i = 0; s && *s && s[lstrlenW(s) + 1]; i++)
    {
        argv[argc++] = own(dyn, &ndyn, kv("--filter=", utf8(s, -1)));
        s += lstrlenW(s) + 1;
        argv[argc++] = own(dyn, &ndyn, kv("--filter-patterns=", utf8(s, -1)));
        s += lstrlenW(s) + 1;
    }
    if (i && ofn->nFilterIndex)
    {
        wsprintfA(idx, "--filter-index=%lu", ofn->nFilterIndex);
        argv[argc++] = idx;
    }
    argv[argc] = NULL;

    for (i = 0; i < argc; i++) if (!argv[i]) goto out;  /* 할당·변환 실패 */
    do  /* 덮어쓰기 확인에서 '아니요'면 다시 띄움 */
    {
        release(res);
        res = NULL;
        fallback = TRUE;
        code = run_helper(argv, ofn->hwndOwner);
        if (code == 10) { ret = done(FALSE, 0); fallback = FALSE; }  /* 취소 */
        else if (code == 0 && (res = read_file(tmp)))
        {
            ret = fill(ofn, res, save);
            fallback = !ret && g_err == CDERR_GENERALCODES;  /* 결과 경로 없음·변환 실패 */
        }
    } while (!ret && !fallback && g_err == RETRY);
out:
    DeleteFileW(tmp);
    release(res);
    for (i = 0; i < ndyn; i++) release(dyn[i]);
    release(dyn);
    release(argv);
    return fallback ? pass(name, ofn) : ret;  /* 헬퍼 실패 → Wine 대화상자 */
}

BOOL WINAPI GetOpenFileNameW(OPENFILENAMEW *ofn) { return portal_dialog(ofn, FALSE); }
BOOL WINAPI GetSaveFileNameW(OPENFILENAMEW *ofn) { return portal_dialog(ofn, TRUE); }

DWORD WINAPI CommDlgExtendedError(void)
{
    DWORD (WINAPI *f)(void);
    if (g_ours) return g_err;
    f = (void *)wine_proc("CommDlgExtendedError");
    return f ? f() : 0;
}

/* comdlg32·explorer 대역 자체검사. test/run.sh 가 앱 폴더에 설치한 뒤 이렇게 실행:
   WINEPREFIX="$PFX" WDI_TEST_DIR=<빈 폴더> WINE_DESKTOP_INTEGRATION_HELPER="$PWD/test/fake-helper.sh" wine "$APP/test.exe" */
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>
#include <shellapi.h>

extern char * CDECL wine_get_unix_file_name(LPCWSTR dos);
extern WCHAR * CDECL wine_get_dos_file_name(LPCSTR unix);
extern WCHAR * __cdecl wcsrchr(const WCHAR *, WCHAR);

static int bad, hooked, boxed;
static WCHAR args_path[MAX_PATH], count_path[MAX_PATH];
static WCHAR tdir[MAX_PATH];     /* 시험 폴더 $WDI_TEST_DIR 의 DOS 경로 */
static char tdir_unix[MAX_PATH];

static void out(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, 0); }
static void check(int ok, const char *what) { out(ok ? "OK   " : "FAIL "); out(what); out("\n"); if (!ok) bad = 1; }

/* 파일 내용 (없으면 NULL). 끝에 NUL 두 개라 WCHAR 로도 읽힘 */
static const char *read_all(const WCHAR *path)
{
    static char buf[4096];
    DWORD got = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    ReadFile(f, buf, sizeof(buf) - 2, &got, NULL);
    CloseHandle(f);
    buf[got] = buf[got + 1] = 0;
    return buf;
}

/* 시험 폴더 안 name 의 DOS 경로를 buf 에 */
static WCHAR *at(WCHAR *buf, const WCHAR *name)
{
    lstrcpyW(buf, tdir);
    lstrcatW(buf, L"\\");
    lstrcatW(buf, name);
    return buf;
}

/* 가짜 헬퍼가 기록한 인자 (없으면 NULL) */
static const char *helper_args(void) { return read_all(args_path); }

/* explorer 시험: 카톡처럼 ShellExecute("explorer", params) → 앱 폴더의 우리 explorer.exe. 종료코드 */
static DWORD explorer(const WCHAR *params, const WCHAR *record)
{
    SHELLEXECUTEINFOW sei;
    WCHAR argfile[MAX_PATH];
    DWORD code = 99;
    DeleteFileW(at(argfile, L"explorer-args"));
    DeleteFileW(record);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"explorer";
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess)
    {
        WaitForSingleObject(sei.hProcess, 30000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    return code;
}

/* 가짜 헬퍼가 "<cmd>\n<dos 의 유닉스 경로>\n" 을 받았는지 */
static int showed(const char *cmd, const WCHAR *dos)
{
    char want[1024];
    WCHAR argfile[MAX_PATH];
    char *u = wine_get_unix_file_name(dos);
    const char *got = read_all(at(argfile, L"explorer-args"));
    if (!u) return 0;
    wsprintfA(want, "%s\n%s\n", cmd, u);
    HeapFree(GetProcessHeap(), 0, u);
    return got && !lstrcmpA(got, want);
}

/* 원본 대신 불린 test.exe 가 기록한 명령줄이 "\"<self>\"" + tail 인지. explorer 는 원본을 기다리지 않으므로 기록될 때까지 기다림 */
static int passed(const WCHAR *record, const WCHAR *self, const WCHAR *tail)
{
    static WCHAR want[1024];
    const WCHAR *got;
    int i;
    lstrcpyW(want, L"\"");
    lstrcatW(want, self);
    lstrcatW(want, L"\"");
    lstrcatW(want, tail);
    for (i = 0; i < 100; i++, Sleep(100))
        if ((got = (const WCHAR *)read_all(record)) && !lstrcmpW(got, want)) return 1;
    return 0;
}
/* 원본이 불리지 않았는지. 원본은 비동기로 뜨므로 기록할 틈을 준 뒤 확인 */
static int absent(const WCHAR *record) { Sleep(2000); return !read_all(record); }
static int args_are(const char *want) { const char *a = helper_args(); return a && !lstrcmpA(a, want); }

static void init(OPENFILENAMEW *o, WCHAR *buf, DWORD n, const WCHAR *title, DWORD flags)
{
    ZeroMemory(o, sizeof(*o));
    o->lStructSize = sizeof(*o);
    o->lpstrFile = buf;
    o->nMaxFile = n;
    o->lpstrTitle = title;
    o->Flags = flags;
    buf[0] = 0;
    DeleteFileW(args_path);
}

static UINT_PTR CALLBACK hook(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG) { hooked = 1; PostMessageW(GetParent(h), WM_COMMAND, IDCANCEL, 0); }
    return 0;
}

/* 덮어쓰기 확인 창을 찾아 '아니요' */
static DWORD WINAPI answer_no(void *p)
{
    HWND w;
    int i;
    for (i = 0; i < 300; i++, Sleep(100))
        if ((w = FindWindowW(L"#32770", L"overwrite"))) { boxed = 1; PostMessageW(w, WM_COMMAND, IDNO, 0); return 0; }
    return 0;
}

void WINAPI start(void)
{
    WCHAR buf[512], title[64], self[MAX_PATH], record[MAX_PATH], ours_explorer[MAX_PATH];
    static WCHAR want[MAX_PATH], multi[MAX_PATH];  /* 스택 4KB 넘으면 __chkstk 필요(-nostdlib) */
    static char wantA[1024];
    OPENFILENAMEW o;
    BOOL r;
    DWORD n, len;
    WCHAR *dos;

    /* 원본 explorer 대역으로 불린 경우: 명령줄을 기록하고 끝 */
    if (GetEnvironmentVariableW(L"WDI_TEST_RECORD", record, MAX_PATH))
    {
        HANDLE f = CreateFileW(record, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        WriteFile(f, GetCommandLineW(), lstrlenW(GetCommandLineW()) * sizeof(WCHAR), &n, NULL);
        CloseHandle(f);
        ExitProcess(0);
    }

    if (!GetEnvironmentVariableA("WDI_TEST_DIR", tdir_unix, MAX_PATH) || !(dos = wine_get_dos_file_name(tdir_unix)))
    {
        out("FAIL WDI_TEST_DIR 없음\n");
        ExitProcess(1);
    }
    lstrcpyW(tdir, dos);
    HeapFree(GetProcessHeap(), 0, dos);
    len = lstrlenW(tdir) + 1;  /* 시험 폴더 안 파일의 nFileOffset */
    /* 다중 선택 기대값: 폴더\0a.txt\0b.txt\0\0 */
    ZeroMemory(multi, sizeof(multi));
    lstrcpyW(multi, tdir);
    lstrcpyW(multi + len, L"a.txt");
    lstrcpyW(multi + len + 6, L"b.txt");

    GetTempPathW(MAX_PATH, args_path);
    lstrcpyW(count_path, args_path);
    lstrcatW(args_path, L"wpf-fake-args");
    lstrcatW(count_path, L"wpf-fake-count");

    /* (a) 단일 선택 */
    init(&o, buf, 512, L"single", OFN_EXPLORER);
    o.lpstrFilter = L"사진\0*.jpg;*.png\0모든 파일\0*.*\0";
    o.nFilterIndex = 1;
    o.lpstrInitialDir = L"Z:\\tmp";
    r = GetOpenFileNameW(&o);
    check(r && !lstrcmpW(buf, at(want, L"photo.jpg")), "(a) 단일 선택 → <시험 폴더>\\photo.jpg");
    check(o.nFileOffset == len && o.nFileExtension == len + 6, "(a) nFileOffset nFileExtension");
    check(o.nFilterIndex == 2, "(a) nFilterIndex 갱신=2");
    check(args_are("--mode=open\n--out=OUT\n--title=single\n--dir=/tmp\n--filter=사진\n--filter-patterns=*.jpg;*.png\n"
                   "--filter=모든 파일\n--filter-patterns=*.*\n--filter-index=1\n"), "(a) 헬퍼 인자");

    /* (b) 다중 선택 */
    init(&o, buf, 512, L"multi", OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR);
    o.nFilterIndex = 7;
    r = GetOpenFileNameW(&o);
    check(r && !memcmp(buf, multi, (len + 13) * sizeof(WCHAR)), "(b) 다중 선택 → 폴더\\0a.txt\\0b.txt\\0\\0");
    check(o.nFileOffset == len && o.nFileExtension == 0, "(b) nFileOffset nFileExtension=0");
    check(o.nFilterIndex == 7, "(b) 필터 번호 0이면 nFilterIndex 유지");
    check(args_are("--mode=open\n--out=OUT\n--title=multi\n--multiple\n"), "(b) 헬퍼 인자");

    /* (g) 다중 선택 플래그 + 1개 → 경로\0\0 */
    init(&o, buf, 512, L"single", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    memset(buf, 0xAB, sizeof(buf));
    r = GetOpenFileNameW(&o);
    check(r && !lstrcmpW(buf, at(want, L"photo.jpg")) && !buf[len + 9 + 1], "(g) 다중 플래그 1개 → 경로\\0\\0");
    check(o.nFileOffset == len && o.nFileExtension == len + 6, "(g) nFileOffset nFileExtension");
    init(&o, buf, len + 9 + 1, L"single", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == FNERR_BUFFERTOOSMALL && *(WORD *)buf == len + 9 + 2,
          "(g) 버퍼 경로+1 → 부족, 필요 길이 경로+2");

    /* (h) 다중 선택인데 폴더가 다르면 첫 파일만 */
    init(&o, buf, 512, L"mixed", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    memset(buf, 0xAB, sizeof(buf));
    r = GetOpenFileNameW(&o);
    check(r && !lstrcmpW(buf, at(want, L"a.txt")) && !buf[len + 5 + 1] && o.nFileOffset == len,
          "(h) 폴더 다름 → 첫 파일 단일 결과");

    /* (e) 버퍼 부족 */
    init(&o, buf, 5, L"single", OFN_EXPLORER);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == FNERR_BUFFERTOOSMALL && *(WORD *)buf == len + 9 + 1,
          "(e) FNERR_BUFFERTOOSMALL, 필요 길이 경로+1");

    /* (c) 취소: 직전 오류가 남지 않아야 함 */
    init(&o, buf, 512, L"cancel", OFN_EXPLORER);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == 0, "(c) 취소 → FALSE, CommDlgExtendedError()==0");

    /* (d) 저장: 확장자 없는 이름 + lpstrDefExt */
    init(&o, buf, 512, L"save", OFN_EXPLORER);
    at(buf, L"KakaoTalk_20261023_124208637.jpg");
    o.lpstrInitialDir = L"Z:\\tmp";  /* lpstrFile 의 폴더가 우선해야 함 */
    o.lpstrDefExt = L"jpg";
    o.lpstrFileTitle = title;
    o.nMaxFileTitle = 64;
    r = GetSaveFileNameW(&o);
    check(r && !lstrcmpW(buf, at(want, L"noext.jpg")), "(d) 저장 → noext.jpg 덧붙임");
    check(o.nFileOffset == len && o.nFileExtension == len + 6 && !lstrcmpW(title, L"noext.jpg"), "(d) 오프셋, lpstrFileTitle");
    wsprintfA(wantA, "--mode=save\n--out=OUT\n--title=save\n--dir=%s/\n--name=KakaoTalk_20261023_124208637.jpg\n", tdir_unix);
    check(args_are(wantA), "(d) 헬퍼 인자 (lpstrFile 폴더 우선, --name)");

    /* (i) 덧붙인 이름이 이미 있음 + OFN_OVERWRITEPROMPT → 확인 창, '아니요' → 다시 띄움 */
    init(&o, buf, 512, L"overwrite", OFN_EXPLORER | OFN_OVERWRITEPROMPT);
    DeleteFileW(count_path);
    o.lpstrDefExt = L"jpg";
    CloseHandle(CreateThread(NULL, 0, answer_no, NULL, 0, NULL));
    r = GetSaveFileNameW(&o);
    check(r && boxed && !lstrcmpW(buf, at(want, L"noext.jpg")), "(i) 덮어쓰기 확인 '아니요' → 재선택 noext.jpg");

    /* (f) 훅 → Wine 대화상자로 넘김(훅이 바로 취소), 헬퍼는 불리지 않음 */
    init(&o, buf, 512, L"single", OFN_EXPLORER | OFN_ENABLEHOOK);
    o.lpfnHook = hook;
    r = GetOpenFileNameW(&o);
    check(!r && hooked && !helper_args(), "(f) OFN_ENABLEHOOK → Wine 대화상자, 헬퍼 미호출");

    /* (j) 중계: 우리 모듈과 별개인 system32 원본으로 감 */
    {
        WCHAR path[MAX_PATH];
        HMODULE ours = NULL, orig;
        HRESULT (WINAPI *gco)(REFCLSID, REFIID, void **);
        HRESULT (WINAPI *gco_orig)(REFCLSID, REFIID, void **);
        static const GUID bogus = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
        static const GUID iunk = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};  /* IID_IUnknown (libuuid 없이) */
        void *obj = NULL;

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)GetOpenFileNameW, &ours);
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\comdlg32.dll");
        orig = LoadLibraryW(path);
        check(ours && orig && ours != orig, "(j) 우리 DLL 과 system32 원본이 별도 모듈");
        check(GetFileTitleW(L"C:\\a\\b.txt", title, 64) == 0 && !lstrcmpW(title, L"b.txt"), "(j) GetFileTitleW 중계");
        gco = (void *)GetProcAddress(ours, "DllGetClassObject");
        gco_orig = orig ? (void *)GetProcAddress(orig, "DllGetClassObject") : NULL;
        check(gco && gco_orig && gco != gco_orig && gco(&bogus, &iunk, &obj) == CLASS_E_CLASSNOTAVAILABLE,
              "(j) DllGetClassObject 중계 → 원본 응답 CLASS_E_CLASSNOTAVAILABLE");

        /* (k) 넘김 + CommDlgExtendedError 도 원본 값 */
        init(&o, buf, 512, L"single", OFN_EXPLORER);
        o.lStructSize = 0;
        r = GetOpenFileNameW(&o);
        check(!r && CommDlgExtendedError() == CDERR_STRUCTSIZE && !helper_args(), "(k) lStructSize 이상 → 원본, CDERR_STRUCTSIZE");
    }

    /* (l) explorer.exe: 원본 explorer 자리에 이 test.exe 를 둬서(WDI_EXPLORER_ORIGINAL) 넘김을 GUI 없이 확인 */
    GetModuleFileNameW(NULL, self, MAX_PATH);
    lstrcpyW(ours_explorer, self);
    lstrcpyW(wcsrchr(ours_explorer, '\\') + 1, L"explorer.exe");
    /* Wine 의 ShellExecute 는 맨 이름을 현재 폴더 → 시스템 경로 순으로 찾고 exe 폴더는 안 봄.
       앱이 자기 폴더에서 실행된 상황을 만든다 (못 하면 원본 explorer 창이 뜨므로 중단) */
    *wcsrchr(self, '\\') = 0;
    if (!SetCurrentDirectoryW(self) || GetFileAttributesW(L"explorer.exe") == INVALID_FILE_ATTRIBUTES)
    {
        out("FAIL 앱 폴더에 explorer.exe 없음\n");
        ExitProcess(1);
    }
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetTempPathW(MAX_PATH, record);
    lstrcatW(record, L"wdi-record");
    SetEnvironmentVariableW(L"WDI_EXPLORER_ORIGINAL", self);
    SetEnvironmentVariableW(L"WDI_TEST_RECORD", record);
    check(explorer(L"/select,C:\\windows\\win.ini", record) == 0 && showed("show-items", L"C:\\windows\\win.ini") &&
          absent(record), "(l) /select,C:\\windows\\win.ini → show-items");
    check(explorer(L"/SELECT, \"C:\\Program Files\\x y.txt\" ", record) == 0 &&
          showed("show-items", L"C:\\Program Files\\x y.txt"), "(l) /SELECT, \"공백 경로\" → show-items");
    check(explorer(L"\"C:\\Program Files\"", record) == 0 && showed("show-folders", L"C:\\Program Files"),
          "(l) 폴더 하나 → show-folders");
    check(explorer(L"/select,C:\\windows\\nonexistent\\x.txt", record) == 0 && showed("show-folders", L"C:\\windows") &&
          absent(record), "(l) 부모 폴더 없음 → 있는 가장 가까운 상위 폴더 show-folders");
    check(explorer(L"/select,Q:\\nonexistent\\x.txt", record) == 0 && passed(record, self, L" /select,Q:\\nonexistent\\x.txt"),
          "(l) 상위 폴더도 없음(없는 드라이브) → 원본");
    check(explorer(NULL, record) == 0 && passed(record, self, L"") && !helper_args(), "(l) 인자 없음 → 원본");
    check(explorer(L"/n,C:\\windows", record) == 0 && passed(record, self, L" /n,C:\\windows"), "(l) /n,... → 원본에 인자 그대로");
    check(explorer(L"C:\\windows\\win.ini", record) == 0 && passed(record, self, L" C:\\windows\\win.ini"),
          "(l) 파일 하나(폴더 아님) → 원본");
    SetEnvironmentVariableW(L"WINE_DESKTOP_INTEGRATION_HELPER", L"/nonexistent/helper");
    check(explorer(L"/select,C:\\windows\\win.ini", record) == 0 &&
          passed(record, self, L" /select,C:\\windows\\win.ini"), "(l) 헬퍼 실패 → 원본에 인자 그대로");
    SetEnvironmentVariableW(L"WDI_EXPLORER_ORIGINAL", ours_explorer);
    check(explorer(NULL, record) == 1 && absent(record), "(l) 원본 경로가 자기 자신 → 재귀 없이 실패");

    out(bad ? "FAIL\n" : "ALL OK\n");
    ExitProcess(bad);
}

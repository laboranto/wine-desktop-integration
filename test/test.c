/* 프록시 자체검사. 실행 (exe 를 저장소 밖 폴더에 복사해서 그 폴더에 설치):
   ./build.sh && mkdir -p "$APP" && cp test/test.exe "$APP/" && ./install.sh "$PFX" "$APP/test.exe" &&
   WINEPREFIX="$PFX" WINE_PORTAL_FILEDIALOG_HELPER="$PWD/test/fake-helper.sh" wine "$APP/test.exe" */
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>

static int bad, hooked, boxed;
static WCHAR args_path[MAX_PATH], count_path[MAX_PATH];

static void out(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, lstrlenA(s), &n, 0); }
static void check(int ok, const char *what) { out(ok ? "OK   " : "FAIL "); out(what); out("\n"); if (!ok) bad = 1; }

/* 가짜 헬퍼가 기록한 인자 (없으면 NULL) */
static const char *helper_args(void)
{
    static char buf[4096];
    DWORD got = 0;
    HANDLE f = CreateFileW(args_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    return buf;
}
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
    static const WCHAR multi[] = L"Z:\\tmp\\wpf-test\0a.txt\0b.txt\0";
    WCHAR buf[512], title[64];
    OPENFILENAMEW o;
    BOOL r;

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
    check(r && !lstrcmpW(buf, L"Z:\\tmp\\wpf-test\\photo.jpg"), "(a) 단일 선택 → Z:\\tmp\\wpf-test\\photo.jpg");
    check(o.nFileOffset == 16 && o.nFileExtension == 22, "(a) nFileOffset=16 nFileExtension=22");
    check(o.nFilterIndex == 2, "(a) nFilterIndex 갱신=2");
    check(args_are("--mode=open\n--out=OUT\n--title=single\n--dir=/tmp\n--filter=사진\n--filter-patterns=*.jpg;*.png\n"
                   "--filter=모든 파일\n--filter-patterns=*.*\n--filter-index=1\n"), "(a) 헬퍼 인자");

    /* (b) 다중 선택 */
    init(&o, buf, 512, L"multi", OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR);
    o.nFilterIndex = 7;
    r = GetOpenFileNameW(&o);
    check(r && !memcmp(buf, multi, sizeof(multi)), "(b) 다중 선택 → 폴더\\0a.txt\\0b.txt\\0\\0");
    check(o.nFileOffset == 16 && o.nFileExtension == 0, "(b) nFileOffset=16 nFileExtension=0");
    check(o.nFilterIndex == 7, "(b) 필터 번호 0이면 nFilterIndex 유지");
    check(args_are("--mode=open\n--out=OUT\n--title=multi\n--multiple\n"), "(b) 헬퍼 인자");

    /* (g) 다중 선택 플래그 + 1개 → 경로\0\0 */
    init(&o, buf, 512, L"single", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    memset(buf, 0xAB, sizeof(buf));
    r = GetOpenFileNameW(&o);
    check(r && !memcmp(buf, L"Z:\\tmp\\wpf-test\\photo.jpg\0", 27 * sizeof(WCHAR)), "(g) 다중 플래그 1개 → 경로\\0\\0");
    check(o.nFileOffset == 16 && o.nFileExtension == 22, "(g) nFileOffset=16 nFileExtension=22");
    init(&o, buf, 26, L"single", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == FNERR_BUFFERTOOSMALL && *(WORD *)buf == 27, "(g) 버퍼 26 → 부족, 필요 길이 27");

    /* (h) 다중 선택인데 폴더가 다르면 첫 파일만 */
    init(&o, buf, 512, L"mixed", OFN_EXPLORER | OFN_ALLOWMULTISELECT);
    memset(buf, 0xAB, sizeof(buf));
    r = GetOpenFileNameW(&o);
    check(r && !memcmp(buf, L"Z:\\tmp\\wpf-test\\a.txt\0", 23 * sizeof(WCHAR)) && o.nFileOffset == 16,
          "(h) 폴더 다름 → 첫 파일 단일 결과");

    /* (e) 버퍼 부족 */
    init(&o, buf, 5, L"single", OFN_EXPLORER);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == FNERR_BUFFERTOOSMALL && *(WORD *)buf == 26, "(e) FNERR_BUFFERTOOSMALL, 필요 길이 26");

    /* (c) 취소: 직전 오류가 남지 않아야 함 */
    init(&o, buf, 512, L"cancel", OFN_EXPLORER);
    r = GetOpenFileNameW(&o);
    check(!r && CommDlgExtendedError() == 0, "(c) 취소 → FALSE, CommDlgExtendedError()==0");

    /* (d) 저장: 확장자 없는 이름 + lpstrDefExt */
    init(&o, buf, 512, L"save", OFN_EXPLORER);
    lstrcpyW(buf, L"Z:\\tmp\\wpf-test\\KakaoTalk_20261023_124208637.jpg");
    o.lpstrInitialDir = L"Z:\\tmp";  /* lpstrFile 의 폴더가 우선해야 함 */
    o.lpstrDefExt = L"jpg";
    o.lpstrFileTitle = title;
    o.nMaxFileTitle = 64;
    r = GetSaveFileNameW(&o);
    check(r && !lstrcmpW(buf, L"Z:\\tmp\\wpf-test\\noext.jpg"), "(d) 저장 → noext.jpg 덧붙임");
    check(o.nFileOffset == 16 && o.nFileExtension == 22 && !lstrcmpW(title, L"noext.jpg"), "(d) 오프셋, lpstrFileTitle");
    check(args_are("--mode=save\n--out=OUT\n--title=save\n--dir=/tmp/wpf-test/\n--name=KakaoTalk_20261023_124208637.jpg\n"),
          "(d) 헬퍼 인자 (lpstrFile 폴더 우선, --name)");

    /* (i) 덧붙인 이름이 이미 있음 + OFN_OVERWRITEPROMPT → 확인 창, '아니요' → 다시 띄움 */
    init(&o, buf, 512, L"overwrite", OFN_EXPLORER | OFN_OVERWRITEPROMPT);
    DeleteFileW(count_path);
    o.lpstrDefExt = L"jpg";
    CloseHandle(CreateThread(NULL, 0, answer_no, NULL, 0, NULL));
    r = GetSaveFileNameW(&o);
    check(r && boxed && !lstrcmpW(buf, L"Z:\\tmp\\wpf-test\\noext.jpg"), "(i) 덮어쓰기 확인 '아니요' → 재선택 noext.jpg");

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

    out(bad ? "FAIL\n" : "ALL OK\n");
    ExitProcess(bad);
}

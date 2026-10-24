#!/bin/sh
# 통합 시험: test/run.sh <시험용 WINEPREFIX>
# 그 prefix 에 앱 폴더(drive_c/wdi-test app)와 reg 키를 만들었다 지운다 — 실제로 쓰는 prefix 에는 돌리지 말 것.
# 앱 목록·헬퍼 사본·.desktop 은 임시 폴더(XDG_DATA_HOME, WDI_BIN_DIR)로 돌려 실제 ~/.local 을 건드리지 않는다.
set -eu
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "사용: $0 <시험용 WINEPREFIX>" >&2; exit 2; }
pfx=$(cd -P "$1" && pwd)
[ "$pfx" != "$(cd -P ~/.wine 2>/dev/null && pwd)" ] || { echo "~/.wine 에는 돌리지 않음" >&2; exit 2; }
t=$(mktemp -d)
app="$pfx/drive_c/wdi-test app"
ukey='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall'
cleanup() { rm -rf "$t" "$app"
            for k in WdiTest WdiTestUnins WdiTestExpand WdiTestPkg WdiTestVar; do wine reg delete "$ukey\\$k" /f >/dev/null 2>&1 || true; done; }
trap cleanup EXIT
export WINEPREFIX="$pfx" XDG_DATA_HOME="$t/data" WDI_BIN_DIR="$t/bin" WDI_TEST_DIR="$t/files"  # 마지막은 가짜 헬퍼·test.exe 용
mkdir "$WDI_TEST_DIR"
w=./wine-desktop-integration
list="$t/data/wine-desktop-integration/apps"
bad=0
check() { if eval "$2"; then echo "OK   $1"; else echo "FAIL $1"; bad=1; fi; }
flush() { timeout 30 wineserver -w || true; }  # reg 변경이 user.reg/system.reg 에 쓰일 때까지
ovr() { grep -A3 -F "AppDefaults\\\\$1\\\\DllOverrides]" "$pfx/user.reg" | grep -q '^"comdlg32"="native,builtin"'; }  # user.reg 에 $1 override

./build.sh
check "헬퍼 file_uri (공백·비 UTF-8·한글)" "python3 -I -c 'import runpy, sys
u = runpy.run_path(sys.argv[1])[\"file_uri\"](\"/tmp/a b/\\udcff한.txt\")
assert u == \"file:///tmp/a%20b/%FF%ED%95%9C.txt\", u' wine-desktop-integration-helper"
mkdir -p "$t/rs/drive_c" && ln -s drive_c "$t/rs/c:" && ln -s /etc/hostname "$t/rs/drive_c/l"
check "헬퍼 resolve (부모 폴더만 풂: 콜론 경로·루트, 파일 이름 링크는 그대로)" "python3 -I -c 'import runpy, sys
r = runpy.run_path(sys.argv[1])[\"resolve\"]; d = sys.argv[2]
assert r(d + \"/c:/l\") == d + \"/drive_c/l\", r(d + \"/c:/l\")
assert r(d + \"/c:/\") == d + \"/drive_c/\", r(d + \"/c:/\")' wine-desktop-integration-helper \"\$(cd -P \"\$t/rs\" && pwd)\""

mkdir -p "$app"
cp test/test.exe "$app/"
cp test/test.exe "$app/other.exe"
printf 'old wine-portal-filedialog proxy' > "$app/comdlg32.dll"  # 옛 버전이 깔려 있던 폴더
$w install "$pfx" "$app/test.exe"
flush
check "옛 버전 프록시를 덮어씀" 'cmp -s comdlg32.dll "$app/comdlg32.dll"'
check "목록 한 줄" '[ "$(cat "$list")" = "$pfx	$app/test.exe" ]'
check "헬퍼 사본" 'cmp -s wine-desktop-integration-helper "$t/bin/wine-desktop-integration-helper"'
$w status | tee "$t/s"
check "status 최신·override" 'grep -qF "test.exe: comdlg32.dll=최신 explorer.exe=최신 override=있음" "$t/s"'
ln -s "$PWD/wine-desktop-integration" "$t/wdi"  # PATH 의 링크로 부른 경우: 산출물은 링크 대상 옆에서 찾아야 함
check "심볼릭 링크로 관리 명령 호출" '"$t/wdi" install "$pfx" "$app/test.exe" > /dev/null && [ "$(wc -l < "$list")" = 1 ]'
mkdir "$t/same" && cp test/test.exe "$t/same/"
check "목록에 없는 앱 uninstall → 거부, 같은 이름 앱의 override·목록 유지" \
    '! $w uninstall "$pfx" "$t/same/test.exe" 2> "$t/e" && grep -qF "목록에 없음" "$t/e" && flush && ovr test.exe && [ "$(wc -l < "$list")" = 1 ]'

WINE_DESKTOP_INTEGRATION_HELPER="$PWD/test/fake-helper.sh" wine "$app/test.exe" | tee "$t/r"
check "test.exe 자체검사" 'grep -qx "ALL OK" "$t/r"'

printf x >> "$app/explorer.exe"
$w status > "$t/s"
check "바뀐 파일 → 다름" 'grep -qF "explorer.exe=다름" "$t/s"'
$w install "$pfx" "$app/test.exe" > /dev/null
check "재설치해도 목록 중복 없음" '[ "$(wc -l < "$list")" = 1 ]'
printf x >> "$app/comdlg32.dll"
printf '%s\t%s\n' "$pfx" "$app/gone.exe" "$t/nopfx" "$t/nopfx/drive_c/a.exe" >> "$list"
$w update 2> "$t/e"
check "update: exe·prefix 없는 항목은 경고만, 목록 유지, prefix 안 만듦" \
    'grep -qF "exe 없음" "$t/e" && grep -qF "prefix 없음" "$t/e" && [ "$(wc -l < "$list")" = 3 ] && [ ! -e "$t/nopfx" ]'
$w status > "$t/s"
check "update 뒤 최신" 'grep -qF "comdlg32.dll=최신 explorer.exe=최신" "$t/s"'
$w uninstall "$pfx" "$app/gone.exe" > /dev/null
check "exe 없는 항목도 uninstall 로 목록에서 제거" '! grep -qF gone.exe "$list" && [ -f "$app/explorer.exe" ]'
$w uninstall "$t/nopfx" "$t/nopfx/drive_c/a.exe" > /dev/null
check "prefix 없는 항목도 uninstall 로 목록에서만 제거" '! grep -qF nopfx "$list" && [ ! -e "$t/nopfx" ]'

mkdir -p "$t/foreign" && printf 'MZ app own' > "$t/foreign/explorer.exe" && cp test/test.exe "$t/foreign/f.exe"
check "앱 자체 explorer.exe 면 중단" '! $w install "$pfx" "$t/foreign/f.exe" 2>/dev/null && [ "$(cat "$t/foreign/explorer.exe")" = "MZ app own" ]'
check "tty 아니면 대화형 install 거부" '! $w install "$pfx" < /dev/null 2>/dev/null'

# scan: .desktop(winemenubuilder 형식) → other.exe, Uninstall 키 → test.exe·other.exe(중복)·언인스톨러(제외)
#       REG_EXPAND_SZ → expand.exe, Package Cache·%변수% 경로는 파일이 있어도 제외
mkdir -p "$t/data/applications/wine/Programs/WDI"
sed "s|@PFX@|$pfx|" > "$t/data/applications/wine/Programs/WDI/other.desktop" <<'EOF'
[Desktop Entry]
Name=WDI Other
Exec=env WINEPREFIX="@PFX@" wine C:\\\\wdi-test\\ app\\\\other.exe
Type=Application
EOF
sed "s|@PFX@|$pfx|" > "$t/data/applications/wine-extension-x.desktop" <<'EOF'
[Desktop Entry]
Name=A Wine application
Exec=env "WINEPREFIX=@PFX@" wine start /ProgIDOpen "X.Doc" %f
EOF
cp test/test.exe "$app/unins000.exe"
wine reg add "$ukey\\WdiTest" /v DisplayName /d "WDI Test" /f >/dev/null
wine reg add "$ukey\\WdiTest" /v DisplayIcon /d '"C:\wdi-test app\test.exe",0' /f >/dev/null
wine reg add "$ukey\\WdiTestUnins" /v DisplayIcon /d 'C:\wdi-test app\unins000.exe' /f >/dev/null
mkdir -p "$app/Package Cache" "$app/%WDI%"
cp test/test.exe "$app/expand.exe" && cp test/test.exe "$app/Package Cache/pkg.exe" && cp test/test.exe "$app/%WDI%/var.exe"
wine reg add "$ukey\\WdiTestExpand" /v DisplayName /d "WDI Expand" /f >/dev/null
wine reg add "$ukey\\WdiTestExpand" /v DisplayIcon /t REG_EXPAND_SZ /d 'C:\wdi-test app\expand.exe' /f >/dev/null
wine reg add "$ukey\\WdiTestPkg" /v DisplayIcon /d 'C:\wdi-test app\Package Cache\pkg.exe' /f >/dev/null
wine reg add "$ukey\\WdiTestVar" /v DisplayIcon /t REG_EXPAND_SZ /d 'C:\wdi-test app\%WDI%\var.exe' /f >/dev/null
flush
$w scan "$pfx" | tee "$t/s"
check "scan 세 개(.desktop·REG_SZ·REG_EXPAND_SZ, 언인스톨러·ProgIDOpen·Package Cache·%변수% 제외), 설치 표시" \
    '[ "$(cat "$t/s")" = "  1)          WDI Expand  $app/expand.exe
  2)          WDI Other  $app/other.exe
  3) [설치됨] WDI Test  $app/test.exe" ]'
$w install --pick 2 "$pfx"
check "--pick 2 → other.exe 추가" '[ "$(cut -f2 "$list" | sort | tr "\n" " ")" = "$app/other.exe $app/test.exe " ]'
check "범위 밖 번호 거부" '! $w install --pick 9 "$pfx" 2>/dev/null'

$w install "$pfx" "$t/same/test.exe" > /dev/null  # 같은 prefix·같은 exe 이름, 다른 폴더
$w uninstall "$pfx" "$app/test.exe"
flush
check "같은 폴더 앱이 남아 있으면 파일 유지" '[ -f "$app/explorer.exe" ] && [ -f "$app/comdlg32.dll" ] && [ -f "$t/bin/wine-desktop-integration-helper" ]'
check "같은 exe 이름 항목이 남아 있으면 override 유지" 'ovr test.exe'
$w uninstall "$pfx" "$t/same/test.exe"
$w uninstall "$pfx" "$app/other.exe"
flush
check "전부 제거: 파일·헬퍼·목록" '[ ! -e "$app/explorer.exe" ] && [ ! -e "$app/comdlg32.dll" ] && [ ! -e "$t/bin/wine-desktop-integration-helper" ] && [ ! -s "$list" ]'
check "override 제거" '! ovr test.exe'
check "앱 exe 는 그대로" '[ -f "$app/test.exe" ]'

[ $bad = 0 ] && echo "ALL OK" || { echo FAIL; exit 1; }

#!/bin/sh
# 사용: install.sh [--uninstall] <WINEPREFIX> <앱 exe 의 유닉스 경로>
# 앱 폴더에 comdlg32.dll(프록시)을 두고 그 앱만 override 한다. 프록시는 system32 의 Wine 원본으로 넘긴다.
# 이미 설치돼 있으면 거의 아무것도 안 하므로 실행기에서 매번 불러도 됨.
# ponytail: 64비트(x86_64) 앱만. 32비트 앱은 i386 빌드와 i386-windows 사본이 필요
set -eu
here=$(cd "$(dirname "$0")" && pwd)
uninstall=0
if [ "${1:-}" = --uninstall ]; then uninstall=1; shift; fi
if [ $# -ne 2 ] || [ -z "$1" ] || [ ! -d "$1/drive_c" ] || [ ! -f "$2" ]; then
    echo "사용: $0 [--uninstall] <WINEPREFIX> <앱 exe 의 유닉스 경로>" >&2
    exit 2
fi
export WINEPREFIX="$1"
appdir=$(dirname "$2")
app=$(basename "$2")
wine=${WINE:-$(command -v wine || true)}
[ -n "$wine" ] || { echo "wine 을 찾을 수 없음" >&2; exit 1; }
key="HKCU\\Software\\Wine\\AppDefaults\\$app\\DllOverrides"
# 우리 프록시인지(comdlg32_wine 은 옛 버전 표지). 앱 자체 comdlg32.dll 은 건드리지 않기 위함
ours() { grep -aqE 'wine-portal-filedialog|comdlg32_wine' "$1"; }

if [ $uninstall = 1 ]; then
    if [ -f "$appdir/comdlg32.dll" ] && ours "$appdir/comdlg32.dll"; then
        rm -f "$appdir/comdlg32.dll"
    fi
    rm -f "$appdir/comdlg32_wine.dll"  # 옛 버전이 남긴 것
    "$wine" reg delete "$key" /v comdlg32 /f >/dev/null 2>&1 || true
    echo "제거 완료: $app"
    exit 0
fi

[ -f "$here/comdlg32.dll" ] || { echo "comdlg32.dll 없음: build.sh 먼저 실행" >&2; exit 1; }
if [ -f "$appdir/comdlg32.dll" ] && ! ours "$appdir/comdlg32.dll"; then
    echo "$appdir/comdlg32.dll 은 앱 자체 파일로 보임: 중단" >&2
    exit 1
fi
# 다르면 임시 파일에 쓴 뒤 mv (실행 중 매핑된 파일을 truncate 하지 않게)
put() {
    cmp -s "$1" "$2" && return 0
    cp -f "$1" "$2.tmp.$$" && mv -f "$2.tmp.$$" "$2"
}
put "$here/comdlg32.dll" "$appdir/comdlg32.dll"
rm -f "$appdir/comdlg32_wine.dll"  # 옛 버전이 남긴 링크/사본

# 이미 등록돼 있으면 wine 을 띄우지 않음
if ! grep -iA100 -F "[Software\\\\Wine\\\\AppDefaults\\\\$app\\\\DllOverrides]" "$WINEPREFIX/user.reg" 2>/dev/null |
        awk 'NR > 1 && /^\[/ { exit } 1' | grep -q '^"comdlg32"="native,builtin"'; then
    "$wine" reg add "$key" /v comdlg32 /d native,builtin /f >/dev/null
fi
echo "설치 완료: $app → comdlg32=native,builtin"
echo "헬퍼 $here/wine-portal-filedialog 를 PATH(예: ~/.local/bin)에 두거나 WINE_PORTAL_FILEDIALOG_HELPER 로 지정할 것"

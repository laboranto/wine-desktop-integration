[English](README.md) | **한국어**

# wine-desktop-integration

Wine을 다시 빌드하지 않고, 리눅스에서 Wine 앱 하나하나를 데스크톱과 연결해 준다.

- **파일 대화상자**: `GetOpenFileNameW` / `GetSaveFileNameW`가 xdg-desktop-portal을 거쳐 데스크톱의 파일 선택기(KDE, GNOME 등)를 띄운다.
- **"폴더 열기"**: `explorer /select,<파일>`과 `explorer <폴더>`가 `org.freedesktop.FileManager1`을 거쳐 리눅스 파일 관리자(Dolphin, Nautilus 등)를 연다.

처리할 수 없는 호출은 Wine 원래 구현으로 넘긴다. 잘 안 되더라도 평소 Wine 대화상자가 뜨는 데서 그친다.

KDE Plasma 6에서 wine-cachyos 10.0과 카카오톡으로 시험했다.

## 원리

Windows와 Wine은 DLL을 `system32`보다 프로그램 exe가 있는 폴더에서 먼저 찾는다. 앱 exe 옆에 `comdlg32.dll`을 두고 그 앱에만 "네이티브 먼저"(`comdlg32=native,builtin`)를 걸면, 그 앱만 우리 DLL을 쓴다. 같은 prefix의 다른 앱은 그대로다.

- `comdlg32.dll`은 작은 대리 DLL이다. 파일 대화상자 호출 두 개만 직접 처리한다. 처리는 리눅스 쪽 Python 스크립트 `wine-desktop-integration-helper`를 실행해 포털에 요청하는 방식이다. 나머지 호출은 전부 prefix의 원래 `system32\comdlg32.dll`로 넘긴다.
- `explorer.exe`도 같은 방식이다. 앱이 `explorer`를 실행하면 Wine은 현재 폴더부터 찾는다. 현재 폴더가 앱 설치 폴더라면 우리 사본이 실행돼 파일 관리자로 넘긴다. 모르는 인자는 Wine의 진짜 `explorer.exe`로 넘긴다.

Wine 자체를 고치지 않으므로 Wine을 업데이트해도 계속 동작한다.

## 필요한 것

**빌드**
- `clang`, `lld` (mingw는 필요 없다)
- Wine 개발 파일: `/usr/include/wine/windows`, `/usr/lib/wine/x86_64-windows` (Arch에서는 일반 `wine` 패키지에 들어 있다)

**실행**
- 64비트 Wine prefix와 64비트 앱
- `dbus-python`, `PyGObject`가 설치된 `python3`
- FileChooser 백엔드가 있는 `xdg-desktop-portal` (예: `xdg-desktop-portal-kde`, `-gnome`)
- `org.freedesktop.FileManager1`을 지원하는 파일 관리자 (Dolphin, Nautilus, Nemo 등)

## 설치

```sh
./build.sh
```

그다음 관리 스크립트를 쓴다. 파일을 링크하지 않고 **복사**한다. 그래서 설치한 뒤에는 이 저장소를 옮기거나 지워도 계속 동작한다.

```sh
# prefix 안의 설치 후보 보기 (Wine 메뉴 항목과 Uninstall 레지스트리에서 찾음)
./wine-desktop-integration scan ~/.wine

# 그 목록에서 골라 설치
./wine-desktop-integration install ~/.wine

# 또는 exe를 직접 지정
./wine-desktop-integration install ~/.wine ~/.wine/drive_c/Program\ Files/App/App.exe
```

`install`은 다음 일을 한다.
- 앱 폴더에 `comdlg32.dll`과 `explorer.exe`를 복사한다.
- 그 앱에만 override를 건다.
- 헬퍼를 `~/.local/bin`에 복사한다. 위치는 `WDI_BIN_DIR`로 바꿀 수 있고, 그 폴더는 `PATH`에 들어 있어야 한다.

앱 폴더에 같은 이름의 앱 자체 파일이 있으면 덮어쓰지 않고 멈춘다.

다른 명령:

| 명령 | 하는 일 |
|---|---|
| `status` | 설치한 앱마다 파일과 override가 최신인지 보여 줌 |
| `update` | 목록의 앱을 전부 다시 설치 (`./build.sh` 뒤에, 또는 앱 업데이트가 파일을 지웠을 때) |
| `uninstall <prefix> <exe>` | 그 앱에서 파일과 override를 제거 |

설치한 앱 목록은 `~/.local/share/wine-desktop-integration/apps`에 있다.

## 수동 설치

1. `./build.sh`를 실행한다.
2. `wine-desktop-integration-helper`를 `PATH`에 있는 폴더로 복사한다.
3. 앱 exe가 있는 폴더에 `comdlg32.dll`을 복사한다. "폴더 열기"도 쓰려면 `explorer.exe`도 복사한다.
4. 그 prefix로 `winecfg`를 연다. **프로그램** 탭의 **프로그램 추가**로 앱 exe를 추가한다. 그 앱을 선택한 채로 **라이브러리** 탭에서 `comdlg32`를 추가하고 **네이티브, 내장**으로 바꾼다.

4번은 명령 한 줄로도 된다.

```sh
WINEPREFIX=~/.wine wine reg add 'HKCU\Software\Wine\AppDefaults\App.exe\DllOverrides' /v comdlg32 /d native,builtin /f
```

`explorer.exe`는 override가 필요 없다.

## 한계

- **64비트 전용이다.** 32비트 앱을 지원하려면 i386 빌드를 따로 만들어야 한다.
- **`GetOpenFileNameW` / `GetSaveFileNameW`만 바꾼다.** 다음 경우에는 그대로 Wine 대화상자가 뜬다.
  - ANSI 버전(`...A`)을 쓰는 앱
  - `IFileDialog`를 쓰는 앱 (최근 앱이 많이 쓰는 COM 대화상자)
  - 훅이나 사용자 템플릿을 쓰는 호출
  - `OFN_EXPLORER` 없이 여러 파일을 고르는 옛 방식의 호출
- **explorer 대체는 현재 폴더에 달려 있다.** Wine은 `explorer`를 앱 폴더가 아니라 현재 폴더에서 찾는다. 앱이 설치 폴더를 현재 폴더로 두고 시작하지 않으면 Wine 자체 explorer가 뜬다. 설치 폴더로 `cd`한 뒤 앱을 켜는 실행기를 쓰면 해결된다.
- `WDI_EXPLORER_ORIGINAL`은 시험용이다.

## 시험

```sh
test/run.sh <버려도 되는 WINEPREFIX>
```

그 prefix 안에 시험용 앱을 만들었다 지운다. 앱 목록과 헬퍼는 임시 폴더로 돌린다. 실제로 쓰는 prefix에는 돌리지 말 것.

## 라이선스

Wine과 같은 LGPL-2.1-or-later다. [LICENSE](LICENSE)를 참고하라. 코드는 직접 작성했고 Wine 헤더만 사용한다.

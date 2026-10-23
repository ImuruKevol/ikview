# macOS 파일 연결·Finder·터미널

IKView는 **Finder 우클릭에 표시되는 메뉴**를 제공하는 네이티브 Finder Sync 확장을 포함합니다. 기존 서비스 등록은 제거했습니다. 앱은 `~/Applications/IKView.app`, 명령은 `~/.local/bin/ikview`에 설치합니다. 실행 중인 앱으로 파일 URL을 보내므로 터미널에서 열 때마다 뷰어 프로세스를 새로 만들지 않습니다.

## 설치

저장소에서 다음을 실행합니다.

```sh
./scripts/install-macos-integration.sh
export PATH="$HOME/.local/bin:$PATH"
```

두 번째 줄을 `~/.zshrc`에 추가하면 새 터미널에서도 `ikview`를 사용할 수 있습니다. 셸 설정을 바꾸지 않고 `~/.local/bin/ikview`로 직접 실행해도 됩니다. 관리자 권한이나 별도 Python·Homebrew 설치는 필요 없습니다. 설치기는 Finder 확장을 등록하고 활성화 요청을 보낸 뒤 **등록 경로 / 사용자 선택 상태 / 실제 확장 프로세스 실행 여부**를 각각 출력합니다. 요청 성공을 실제 메뉴 표시 성공으로 간주하지 않습니다.

배포 ZIP만 있다면 압축을 풀고 앱에 포함된 설치기를 실행합니다.

```sh
/bin/sh "/앱이 있는 경로/ikview.app/Contents/Resources/integration/install.sh"
```

설치 경로는 선택할 수 있습니다. 기존의 다른 앱이나 다른 `ikview` 명령, 관리 경로의 심볼릭 링크는 덮어쓰지 않습니다. 같은 앱을 업데이트할 때는 설치된 앱을 종료하세요. 서명 검사·등록·명령 게시가 실패하면 이전 앱과 명령 설정을 복구하고, 복구까지 실패하면 남긴 경로를 출력합니다.

```sh
./scripts/install-macos-integration.sh \
  --destination "$HOME/Applications/IKView.app" \
  --prefix "$HOME/.local"
```

## 터미널에서 열기

```sh
ikview .                         # 현재 터미널 디렉터리
ikview                           # 인자가 없으면 현재 디렉터리
ikview "$HOME/Pictures/여행"       # 폴더
ikview "사진 1.jpg" "사진 2.png"   # 여러 파일
ikview "사진 모음.cbz"            # 압축 이미지 모음
ikview -- ./-이름이특이한사진.png   # 옵션처럼 시작하는 파일명
```

일반 파일 인자는 `/usr/bin/open -a`로 설치된 앱에 전달합니다. 공백·따옴표·개행·셸 특수문자가 있는 파일명도 인자를 나눠서 처리하며 셸 코드로 평가하지 않습니다. `--inspect`, `--screenshot` 등 진단 옵션은 실행 파일에 직접 전달하므로 별도 프로세스로 실행됩니다.

## Finder 우클릭

이미지·PDF·압축파일·폴더를 선택하고 우클릭하면 메뉴에 바로 표시됩니다. `서비스` 하위 항목이 아닙니다. 다른 Finder 확장과의 상대적 위치는 Finder가 정합니다.

| 메뉴 | 동작 |
|---|---|
| IKView로 열기 | 선택 이미지·PDF·이미지 압축파일·폴더 열기. 여러 항목 선택 가능 |
| IKView로 폴더 열기 | 파일이면 그 파일의 폴더, 폴더이면 선택 폴더 자체 열기. 같은 폴더는 한 번만 열기 |

후자는 README 같은 일반 파일을 선택했을 때도 그 파일이 있는 이미지 폴더를 열 수 있습니다. 선택 파일과 부모 폴더의 macOS 접근 권한은 다를 수 있어 보호 폴더에서는 시스템이 접근을 요청하거나 폴더를 다시 선택해야 할 수 있습니다.

Finder 창의 빈 배경에서는 **IKView로 폴더 열기**가 현재 폴더를 엽니다. 사이드바 폴더에서는 우클릭한 폴더를 엽니다. 메뉴가 뜬 뒤 다른 창을 클릭해도 원래 메뉴를 연 선택 항목이 유지됩니다. 지원하지 않는 일반 파일에서는 포함한 폴더 열기만 제공합니다.

```sh
ikview --finder-status    # 읽기 전용 상태 확인
ikview --finder-enable    # 확장 재등록 + 사용 요청
ikview --finder-disable   # Finder 확장만 끄기
```

활성화 후 컨텍스트 메뉴를 닫고 폴더로 이동하면 Finder가 확장을 불러올 수 있습니다. 여전히 안 보이면 **시스템 설정 → 일반 → 로그인 항목 및 확장 프로그램 → Finder 확장 프로그램**에서 IKView Finder를 확인합니다. macOS 버전에 따라 경로가 다를 수 있습니다. 설치기는 Finder를 강제 종료하거나 LaunchServices 데이터베이스를 초기화하지 않습니다.

현재 개발용 배포는 **ad hoc 로컬 서명**입니다. 서명 무결성 검사는 통과해도 macOS가 확장 실행을 거절할 수 있습니다. `User election: use`는 사용 요청 상태이며 `Extension process running: no`이면 실제 로드 성공은 확인되지 않은 상태입니다. 필요한 경우 인증서 서명으로 다시 패키징합니다.

```sh
IKVIEW_CODESIGN_IDENTITY="Developer ID Application: 개발자 이름 (TEAMID)" ./scripts/package-macos.sh
```

인증서가 키체인에 있어야 합니다. 이 옵션은 공증을 자동으로 수행하지 않습니다. [Apple Finder Sync 확장과 컨텍스트 메뉴 규칙](https://developer.apple.com/library/archive/documentation/General/Conceptual/ExtensibilityPG/Finder.html)

## 기본 이미지 앱 설정

설치는 Finder의 “다음으로 열기” 후보와 Finder 확장을 등록합니다. 기본 앱으로 바꿀 형식은 명령에서 직접 지정합니다.

```sh
ikview --default-status jpg,png,webp,heic
ikview --set-default jpg,jpeg,png,gif,webp,heic,tif,tiff,bmp
```

macOS의 기본 연결 단위는 확장자가 아닌 UTI 형식입니다. `jpg`와 `jpeg`처럼 같은 UTI의 별칭은 함께 바뀌며 명령 결과의 `associated_extensions`에 영향 범위를 표시합니다. ZIP·RAR 전체의 기본 앱은 이미지 형식 명령에 포함하지 않습니다. CBZ를 ZIP의 별칭으로 처리해 ZIP 전체를 바꾸지도 않습니다. macOS가 확인 창을 표시하면 해당 창에서 선택을 완료해야 명령이 끝납니다.

변경 전에 이전 연결 앱을 JSON으로 원자적으로 저장합니다. 같은 명령을 다시 실행해도 처음 저장한 이전 앱을 IKView로 덮어쓰지 않습니다. `backup_path`와 형식별 변경·실패 결과가 출력되고 일부 형식만 실패해도 종료 코드는 실패입니다.

```sh
ikview --restore-defaults
```

복원은 아직 IKView가 기본 앱인 형식에만 적용합니다. 그 후 사용자가 다른 앱으로 바꾼 형식은 유지합니다. 이전 앱이 삭제되었거나 백업이 손상되면 적용 전에 오류를 알립니다. 백업은 이전에 실제로 연결되던 앱을 복원하며, macOS 내부의 “명시적 설정이 없었던 상태” 자체를 복원하는 기능은 아닙니다.

별도 백업 경로도 지정할 수 있습니다.

```sh
ikview --set-default png --defaults-backup "$HOME/ikview-defaults.json"
ikview --restore-defaults --defaults-backup "$HOME/ikview-defaults.json"
```

GUI로도 바꿀 수 있습니다. Finder에서 PNG 등 파일 하나를 선택하고 **정보 가져오기(⌘I) → 다음으로 열기 → IKView → 모두 변경**을 선택하면 같은 형식 전체에 적용됩니다. 한 파일만 바꾸려면 모두 변경을 누르지 않습니다. [Apple 기본 앱 변경 안내](https://support.apple.com/guide/mac-help/mh35597/mac)

## 명령 제거

```sh
/bin/sh "$HOME/Applications/IKView.app/Contents/Resources/integration/install.sh" --remove-cli
```

이 명령은 관리 중인 터미널 명령과 경로 파일만 제거합니다. 앱·Finder 확장·기본 연결은 유지합니다. 확장을 끄려면 먼저 `ikview --finder-disable`을 실행합니다. 앱도 제거할 경우 필요한 기본 연결을 먼저 복원한 뒤 Finder에서 앱을 휴지통으로 옮기면 됩니다.

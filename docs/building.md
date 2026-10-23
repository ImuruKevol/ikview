# 빌드와 배포

## 개발 환경

현재 검증 대상은 Apple Silicon · macOS 26 이상입니다. C++20 컴파일러, CMake 3.24 이상, Qt 6.7 이상, libarchive가 필요합니다. Qt는 Core·Gui·Widgets·Quick·Concurrent·PrintSupport·Test 모듈을 사용합니다. Exiv2는 선택 의존성입니다.

```sh
xcode-select --install
brew install cmake qtbase qtdeclarative qtimageformats qtsvg libarchive exiv2
./scripts/build-macos.sh
```

Homebrew 모듈 경로는 CMake에서 처리합니다. 기본 빌드는 Release 최적화와 지원되는 환경의 링크 시 최적화를 사용합니다. 실행 앱은 `build/ikview.app`에 생성됩니다.

이전 macOS나 Intel을 대상으로 빌드하려면 의존성·배포 대상·아키텍처를 함께 맞춰야 합니다. 해당 환경의 실행은 별도 검증이 필요합니다.

## 창을 띄우지 않는 검사

기본 빌드 스크립트는 `desktop-gui` 레이블의 검사를 제외합니다. UI 일부도 Qt 가상 화면에서 검사하려면 다음 명령을 사용합니다.

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure \
  -E '^(canvas_tests|ux_tests|input_tests|preferences_integration_tests|imagefilter_tests)$'
```

현재 화면을 바꾸지 않고 위젯·단축키·변환·PDF·Finder 전달 로직을 검증합니다. Metal 렌더러와 실제 macOS 입력에 대한 검증을 대신하지는 않습니다. 설정과 파일 작업에는 임시 데이터를 사용합니다.

## 실제 데스크톱 검사

다음 명령은 앱 창을 열고 포커스를 이동할 수 있습니다. 다른 작업에 영향을 주지 않는 시점에 실행하세요.

```sh
./scripts/build-macos.sh --with-gui-tests
```

형식별 패키지 실행 검사는 `scripts/package-macos.sh`의 기본 동작입니다. Finder 메뉴의 실제 로드 여부와 시스템 파일 패널은 자동 단위 검사와 구분해 확인합니다.

## 배포 파일 생성

```sh
./scripts/package-macos.sh --skip-gui-smoke
```

`--skip-gui-smoke`는 앱 실행 검사를 생략합니다. 패키징은 엄격한 서명 검사와 번들 라이브러리 의존성 검사를 수행하고, 실행 검사를 생략했다는 사실을 JSON에 기록합니다. 이전 실행 검사 결과를 현재 패키지의 결과로 재사용하지 않습니다.

| 파일 | 내용 |
| --- | --- |
| `dist/ikview.app` | Qt·이미지 코덱·압축 라이브러리와 Finder 확장을 포함한 앱 |
| `dist/ikview-macos-arm64.zip` | 외부 스토리지에 게시할 압축파일 |
| `dist/ikview-package-validation.json` | 버전, 실행 파일·ZIP의 SHA-256, 서명·의존성·실행 검사 결과 |

배포 파일은 Git 저장소에 포함하지 않습니다. ZIP과 검증 JSON을 함께 게시하면 버전과 파일 해시를 확인할 수 있습니다.

현재 기본 서명은 로컬 서명이며 Apple 공증은 수행하지 않습니다. 인증서로 서명하려면 키체인에 등록된 이름을 지정합니다.

```sh
IKVIEW_CODESIGN_IDENTITY="Developer ID Application: 개발자 이름 (TEAMID)" \
  ./scripts/package-macos.sh --skip-gui-smoke
```

이 옵션만으로 공증이 수행되지는 않습니다. 외부 라이브러리의 출처와 고지는 배포 앱의 `Contents/Resources/third-party`에 포함합니다.

## 소스 묶음과 공개 배포

IKView는 GPL-3.0-or-later로 제공합니다. 현재 설치 파일에는 Exiv2와 Qt 등 외부 라이브러리가 포함되므로, 설치 파일을 게시할 때는 해당 버전의 소스·고지도 함께 제공해야 합니다.

패키징 중에는 번들 라이브러리의 원본 소스와 패치를 다운로드하여 SHA-256을 확인합니다. 최초 실행에는 네트워크와 추가 디스크 공간이 필요하며, 검증한 자료는 `build/license-sources/`에 캐시합니다. 캐시와 소스 묶음은 Git에 추가하지 않습니다.

배포할 변경을 커밋하고 앱을 패키징한 뒤 소스 묶음을 만듭니다.

```sh
python3 scripts/package-sources.py
```

생성한 `dist/ikview-0.7.1-source.tar.gz`와 `dist/ikview-source-validation.json`을 설치 ZIP과 함께 게시합니다. 설치 파일 다운로드 위치에 소스 다운로드 링크도 표시해야 합니다. 자세한 조건과 수집 범위는 [소스 제공과 배포 안내](licensing.md)를 확인하세요.

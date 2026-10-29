# 라이선스와 배포

## 적용 범위

IKView의 코드·문서·프로젝트 제작 이미지는 GPL-3.0-or-later로 제공합니다. 즉, GNU GPL 버전 3 또는 수령자가 선택하는 이후 버전의 조건을 적용합니다. 저작권자는 각 기여자이며, 전체 조건과 무보증 조항은 저장소 루트의 [LICENSE](../LICENSE)에 있습니다.

외부 라이브러리·자료에는 해당 권리자의 조건이 적용됩니다. [외부 라이브러리와 고지](../THIRD_PARTY.md)를 함께 확인하세요. 앱으로 열거나 변환한 사용자 파일에 프로젝트 라이선스가 자동 적용되지는 않습니다.

공개 저장소로 설정하는 것과 오픈소스 이용 허락은 별개입니다. 이 프로젝트는 명시적인 라이선스 원문을 제공합니다. [GitHub의 저장소 라이선스 안내](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository)

## 프로젝트 이미지의 출처

| 자료 | 제작 경로 |
| --- | --- |
| `assets/AppIcon.*`와 아이콘 크기별 이미지 | `src/main.cpp`의 `appIcon()`에 있는 도형 기반 디자인 |
| 화면 안의 조작 아이콘과 스타일 SVG | `src/viewericons.cpp`와 `assets/style/`의 도형 정의 |
| `assets/readme-sample.jpg` | `src/main.cpp`의 `makeDemo()`로 생성한 예제 이미지 |
| `docs/images/`의 화면 | `tests/design_tests.cpp`의 공개 문서용 캡처. 프로젝트 예제 이미지와 도형 사용 |

공개 문서는 IKView의 기능과 실제 화면을 설명합니다. 다른 제품의 화면·로고·매뉴얼은 배포 자료에 포함하지 않습니다. 외부 자료를 새로 추가할 때는 출처와 사용 허락을 확인하고 필요한 고지를 남깁니다. 기능 아이디어를 검토한 기록과 다른 저작물의 코드·이미지·설명문을 이용할 권한은 별개입니다. [한국저작권위원회의 아이디어와 표현 안내](https://www.copyright.or.kr/information-materials/common-sense/knowledge-for-netizen/index.do)

## 설치 파일과 해당 소스

현재 설치 파일은 Exiv2를 포함하므로 결합 배포물의 GPL 조건을 준수해야 합니다. Qt와 다른 번들 라이브러리의 고지·소스 제공 조건도 함께 적용됩니다. 동적 연결이나 소스 저장소의 공개만으로 번들 라이브러리에 대한 의무가 없어지는 것은 아닙니다. [GNU의 연결 안내](https://www.gnu.org/licenses/gpl-faq.html#IfLibraryIsGPL), [Qt의 배포 안내](https://www.qt.io/development/open-source-lgpl-obligations)

패키징 도구는 설치된 Homebrew 의존성의 정확한 버전·원본 소스·SHA-256·적용 패치·빌드 레시피를 수집합니다. 원본 압축파일의 해시를 검증하고 고지 원문을 보존합니다. `scripts/package-sources.py`는 이 자료와 커밋된 IKView 소스를 하나의 소스 압축파일로 묶습니다.

```sh
./scripts/package-macos.sh --skip-gui-smoke
python3 scripts/package-sources.py
```

두 번째 명령은 배포할 변경을 커밋한 뒤 실행합니다. 소스 묶음에는 빌드 폴더·설치 앱·개인 설정을 포함하지 않습니다. IKView 빌드 절차는 [빌드 안내](building.md)에 있으며, 라이브러리별 빌드 옵션과 적용 패치는 소스 묶음의 `dependencies/` 아래에 보존합니다.

스토리지에는 다음 자료를 함께 게시합니다.

| 파일 | 내용 |
| --- | --- |
| `ikview-macos-arm64.zip` | 설치 앱 |
| `ikview-0.7.2-source.tar.gz` | 같은 버전의 IKView 소스, 번들 라이브러리 원본 소스·패치·빌드 레시피 |
| `ikview-package-validation.json` | 앱·설치 파일 해시와 검사 결과 |
| `ikview-source-validation.json` | 소스 커밋과 소스 묶음 해시 |

**설치 파일의 다운로드 링크 바로 옆에 해당 소스의 다운로드 링크를 표시하세요.** 서로 다른 서버를 사용한다면 소스를 찾는 경로를 분명하게 안내하고, 필요한 기간 동안 실제로 내려받을 수 있도록 유지해야 합니다. 저장소 기본 브랜치 링크만으로는 특정 설치 파일에 대응하는 버전과 번들 의존성의 소스를 지정할 수 없습니다. [GNU GPL v3 제6조](https://www.gnu.org/licenses/gpl-3.0.html#section6)

소스·패치 다운로드나 해시 검증이 실패하면 해당 버전의 배포 자료를 완성한 것으로 취급하지 않습니다. 의존성이나 패키징을 변경한 경우 고지·소스 묶음을 다시 생성해야 합니다. 자동 수집 결과는 구성 목록을 확인하는 자료이며, 모든 상황의 법률 판단을 대신하지 않습니다.

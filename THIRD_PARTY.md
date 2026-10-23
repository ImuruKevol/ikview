# 외부 라이브러리와 고지

IKView는 Qt·libarchive·Exiv2 등의 외부 라이브러리를 사용합니다. 아래 표는 주요 구성 요소의 안내이며, 각 라이브러리의 원문 고지와 라이선스가 우선합니다. IKView의 GPL-3.0-or-later 지정은 외부 코드의 저작권이나 라이선스를 변경하지 않습니다.

| 구성 요소 | 용도 | 출처와 조건 |
| --- | --- | --- |
| Qt 6 | 화면·Metal 렌더링·이미지 코덱·인쇄 | 사용하는 런타임 모듈은 LGPL-3.0 또는 GPL 조건을 제공합니다. 모듈 내부의 외부 코드에는 별도 조건이 있습니다. [공식 라이선스 안내](https://doc.qt.io/qt-6/licensing.html) |
| libarchive | 압축파일 탐색 | BSD 계열과 구성 파일별 고지. [원문](https://github.com/libarchive/libarchive/blob/master/COPYING) |
| Exiv2 | EXIF·메타데이터 처리 | GPL-2.0-or-later. [0.28.9 원문](https://github.com/Exiv2/exiv2/blob/v0.28.9/LICENSE.txt) |
| FreeType | Qt의 글꼴 처리 | FreeType License. [원문](https://gitlab.freedesktop.org/freetype/freetype/-/blob/VER-2-15-0/docs/FTL.TXT) |
| libjpeg-turbo / Independent JPEG Group | JPEG 코덱 | IJG·BSD·Zlib 조건. [고지](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/3.2.0/LICENSE.md), [IJG 원문](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/3.2.0/README.ijg) |
| GNU gettext의 libintl | 간접 의존성의 국제화 | 라이브러리는 LGPL-2.1-or-later. 같은 배포 소스의 도구에는 GPL 조건도 있습니다. [프로젝트](https://www.gnu.org/software/gettext/) |

## 현재 배포본

0.7.1 배포본은 Qt 6.12.0·libarchive 3.8.10·Exiv2 0.28.9를 동적으로 연결합니다. Exiv2는 소스 빌드에서 선택 의존성이지만 **현재 설치 파일에는 포함되어 있습니다**. GPL 라이브러리와 결합한 배포물에는 GPL 조건을 준수해야 하며, 동적 연결이라는 이유로 이 조건이 없어지지 않습니다. [GNU의 GPL 라이브러리 연결 안내](https://www.gnu.org/licenses/gpl-faq.html#IfLibraryIsGPL)

Qt Core·Gui·Widgets·Quick·Qml·Concurrent·PrintSupport·Network·OpenGL·Svg 관련 라이브러리와 플러그인, 이미지 코덱·압축·TLS 등 간접 의존성이 포함됩니다. 정확한 목록·버전·라이선스 식별자는 배포 앱의 `Contents/Resources/third-party/libraries.json`에 기록합니다. Homebrew의 라이선스 식별자는 패키지 전체를 요약하므로, 실제로 사용한 라이브러리와 도구의 조건은 원문으로 구분해야 합니다.

고지 파일은 `Contents/Resources/third-party/`에, IKView의 라이선스 원문은 `Contents/Resources/LICENSE`에 포함합니다. Qt 모듈 내부의 외부 코드 고지도 수집하며, 사용하지 않는 구성 요소의 고지가 함께 포함될 수 있습니다. Qt의 구성 요소별 안내는 [공식 목록](https://doc.qt.io/qt-6/licenses-used-in-qt.html)을 참고하세요.

## 추가 고지

이 소프트웨어는 FreeType 팀의 작업을 일부 사용합니다. FreeType의 저작권 고지와 원문 조건을 함께 제공합니다.

> This software is based in part on the work of the Independent JPEG Group.

위 문구는 IJG 라이선스에서 요구하는 원문 고지입니다. 외부 프로젝트의 이름은 사용 구성 요소를 식별하기 위해 기재하며, 해당 프로젝트가 IKView를 보증하거나 후원한다는 의미는 아닙니다.

## 소스 제공

라이선스 문서의 동봉만으로 소스 제공 의무가 충족되지는 않습니다. 설치 파일을 배포할 때는 같은 버전의 IKView 소스와 번들 라이브러리의 소스·적용 패치·빌드 정보를 함께 제공해야 합니다. 준비 방법과 다운로드 위치 표기는 [소스 제공과 배포 안내](docs/licensing.md)에 정리했습니다.

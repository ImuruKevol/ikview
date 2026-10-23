# 외부 라이브러리와 고지

IKView는 다음 라이브러리를 동적으로 연결합니다. 원래 고지와 라이선스 문서는 배포 앱의 `Contents/Resources/third-party`에 포함합니다.

| 라이브러리 | 용도 | 출처와 라이선스 |
| --- | --- | --- |
| Qt 6 | 창·메뉴·설정, Metal 장면 그래프, 비동기 처리, 이미지 코덱, 인쇄 | 구성 요소에 따라 LGPL-3.0 / GPL / 상용 조건. [Qt 안내](https://www.qt.io/licensing/open-source-lgpl-obligations) |
| libarchive | 압축파일 열람 | BSD 계열. [프로젝트](https://www.libarchive.org/) |
| Exiv2 | EXIF·메타데이터 처리, 선택 의존성 | GPL-2.0-or-later. [프로젝트](https://exiv2.org/) |

이미지 코덱·압축·TLS 등의 간접 의존성도 함께 포함될 수 있습니다. 패키징 도구는 설치된 Homebrew 의존성의 원문 라이선스를 수집합니다. 사용 구성에는 Qt Core·Gui·Widgets·Quick·Qml 의존성·Concurrent·PrintSupport·Network와 이미지 형식 플러그인이 포함됩니다.

이 문서는 외부 코드의 조건을 바꾸거나 대체하지 않습니다. IKView 자체 소스의 배포 라이선스는 현재 미지정입니다.

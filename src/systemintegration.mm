#include "systemintegration.h"
#import <AppKit/AppKit.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace ik {
QStringList chooseViewerPaths(const QString &initialDirectory, QWidget *) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"이미지·PDF·폴더 열기";
        panel.prompt = @"열기";
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = YES;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.message = @"파일 또는 폴더를 선택하세요. 여러 항목도 함께 열 수 있습니다.";
        NSString *initial = [NSString stringWithCharacters:reinterpret_cast<const unichar *>(initialDirectory.utf16()) length:initialDirectory.size()];
        panel.directoryURL = [NSURL fileURLWithPath:initial isDirectory:YES];
        QStringList paths;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL *url in panel.URLs) if (url.isFileURL) paths.append(QString::fromUtf8(url.path.UTF8String));
        }
        return paths;
    }
}
bool setDesktopWallpaper(const QString &path, QString *error) {
    @autoreleasepool {
        NSString *filename=[NSString stringWithCharacters:reinterpret_cast<const unichar*>(path.utf16()) length:path.size()];
        NSURL *url=[NSURL fileURLWithPath:filename];
        bool success=true;
        for(NSScreen *screen in [NSScreen screens]) {
            NSError *failure=nil;
            if(![[NSWorkspace sharedWorkspace] setDesktopImageURL:url forScreen:screen options:@{} error:&failure]) {
                success=false;
                if(error && failure) *error=QString::fromUtf8([[failure localizedDescription] UTF8String]);
            }
        }
        return success;
    }
}
void inhibitDisplaySleep(bool enabled) {
    static IOPMAssertionID assertion=kIOPMNullAssertionID;
    if(enabled && assertion==kIOPMNullAssertionID)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,kIOPMAssertionLevelOn,CFSTR("ikview image viewing"),&assertion);
    else if(!enabled && assertion!=kIOPMNullAssertionID) {
        IOPMAssertionRelease(assertion);assertion=kIOPMNullAssertionID;
    }
}
}

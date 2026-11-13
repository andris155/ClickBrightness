#pragma once

#include "App.xaml.g.h"

namespace winrt::BrightnessApp::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    };
}

void InitSettingsWindow(bool show);
void ShowSettingsWindow();
void CloseSettingsWindow();
void OnSettingsThemeChanged();
void OnSettingsDisplayChanged();
void ExitApp();

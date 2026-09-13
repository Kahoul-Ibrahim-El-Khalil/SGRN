import { useState, useEffect, useCallback } from "react";
import { Palette, Check } from "lucide-react";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { Button } from "@/components/ui/button";
import { THEME_OPTIONS, applyTheme, getStoredTheme, isTheme, type Theme } from "@/lib/theme";

export default function ThemeToggle() {
    const [currentTheme, setCurrentTheme] = useState<Theme>(() => getStoredTheme());

    useEffect(() => {
        applyTheme(currentTheme);
    }, [currentTheme]);

    const selectTheme = useCallback((theme: Theme) => {
        applyTheme(theme);
        setCurrentTheme(theme);
    }, []);

    const currentThemeOption = THEME_OPTIONS.find((t) => t.id === currentTheme) ?? THEME_OPTIONS[0];

    return (
        <DropdownMenu>
            <DropdownMenuTrigger asChild>
                <Button variant="outline" className="theme-toggle-trigger" aria-label="Select color theme">
                    <Palette size={18} className="text-primary shrink-0" />
                    <div className="theme-toggle-label-group">
                        <span className="theme-toggle-top-label">Theme</span>
                        <span className="theme-toggle-value">{currentThemeOption.name}</span>
                    </div>
                </Button>
            </DropdownMenuTrigger>

            <DropdownMenuContent align="end" className="theme-toggle-content">
                <div className="theme-toggle-header">
                    <span className="theme-toggle-header-text">Appearance</span>
                </div>

                {THEME_OPTIONS.map((theme) => (
                    <DropdownMenuItem
                        key={theme.id}
                        onSelect={(event) => {
                            event.preventDefault();
                            if (isTheme(theme.id)) {
                                selectTheme(theme.id);
                            }
                        }}
                        className={`theme-toggle-item ${currentTheme === theme.id ? "theme-toggle-item-active" : ""}`}
                    >
                        <div className="theme-toggle-preview-group" aria-hidden="true">
                            <div className="theme-toggle-preview-block" style={{ backgroundColor: theme.preview.bg }} />
                            <div className="theme-toggle-preview-block" style={{ backgroundColor: theme.preview.accent }} />
                        </div>
                        <span className="theme-toggle-name">{theme.name}</span>
                        {currentTheme === theme.id && <Check size={16} className="text-primary shrink-0" />}
                    </DropdownMenuItem>
                ))}
            </DropdownMenuContent>
        </DropdownMenu>
    );
}

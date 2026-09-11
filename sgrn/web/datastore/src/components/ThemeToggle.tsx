// @/components/ThemeToggle.tsx
import { useState, useEffect, useCallback } from "react";
import { Palette, Check } from "lucide-react";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { Button } from "@/components/ui/button";
import { THEME_OPTIONS, applyTheme, getStoredTheme, type Theme } from "@/lib/theme";

export default function ThemeToggle() {
    const [currentTheme, setCurrentTheme] = useState<Theme>(getStoredTheme);

    useEffect(() => {
        applyTheme(currentTheme);
    }, [currentTheme]);

    const selectTheme = useCallback((t_theme: Theme) => {
        setCurrentTheme(t_theme);
    }, []);

    const currentThemeOption = THEME_OPTIONS.find((t) => t.id === currentTheme)!;

    return (
        <DropdownMenu>
            <DropdownMenuTrigger asChild>
                <Button variant="outline" className="theme-toggle-trigger">
                    <Palette size={18} className="text-primary" />
                    <div className="theme-toggle-label-group">
                        <span className="theme-toggle-top-label">THEME</span>
                        <span className="theme-toggle-value">{currentThemeOption.name}</span>
                    </div>
                </Button>
            </DropdownMenuTrigger>

            <DropdownMenuContent align="end" className="theme-toggle-content">
                <div className="theme-toggle-header">
                    <span className="theme-toggle-header-text">SELECT THEME</span>
                </div>

                {THEME_OPTIONS.map((theme) => (
                    <DropdownMenuItem
                        key={theme.id}
                        onClick={() => selectTheme(theme.id)}
                        className={`theme-toggle-item ${currentTheme === theme.id ? "theme-toggle-item-active" : ""}`}
                    >
                        {/* Color Preview */}
                        <div className="theme-toggle-preview-group">
                            <div className="theme-toggle-preview-block" style={{ backgroundColor: theme.preview.bg }} />
                            <div className="theme-toggle-preview-block" style={{ backgroundColor: theme.preview.accent }} />
                        </div>

                        {/* Theme Name */}
                        <span className="theme-toggle-name">{theme.name}</span>

                        {/* Active Indicator */}
                        {currentTheme === theme.id && <Check size={14} className="text-primary font-bold" />}
                    </DropdownMenuItem>
                ))}
            </DropdownMenuContent>
        </DropdownMenu>
    );
}

// @/components/Footer.tsx
import React from "react";
import { LogOut } from "lucide-react";
import { Button } from "@/components/ui/button";
import ThemeToggle from "@/components/ThemeToggle";
import { Logo } from "@/components/Logo";

interface FooterProps {
    onSignOut?: () => void;
    showSignOut?: boolean;
    showThemeToggle?: boolean;
    showLogo?: boolean;
    className?: string;
}

export const Footer: React.FC<FooterProps> = ({
    onSignOut,
    showSignOut = false,
    showThemeToggle = true,
    showLogo = false,
    className = "",
}) => (
    <footer className={`flex items-center justify-between p-4 border-t border-border bg-card ${className}`}>
        {/* Left side - Logo or Copyright */}
        {showLogo ? (
            <Logo size="sm" showDivider={true} />
        ) : (
            <div className="flex items-center gap-2 text-xs text-muted-foreground">
                <span>© {new Date().getFullYear()} ENSTI</span>
                <span className="hidden sm:inline">• All rights reserved</span>
            </div>
        )}

        {/* Right side - Controls */}
        <div className="flex items-center gap-4">
            {showThemeToggle && <ThemeToggle />}

            {showSignOut && onSignOut && (
                <Button variant="outline" size="sm" onClick={onSignOut} className="gap-2">
                    <LogOut size={16} />
                    <span className="hidden md:inline">Sign Out</span>
                </Button>
            )}
        </div>
    </footer>
);

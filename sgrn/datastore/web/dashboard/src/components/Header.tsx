// @/components/Header.tsx
import React from "react";
import { Logo } from "@/components/Logo";
import ThemeToggle from "@/components/ThemeToggle";

interface HeaderProps {
    showThemeToggle?: boolean;
    className?: string;
}

export const Header: React.FC<HeaderProps> = ({ showThemeToggle = true, className = "" }) => (
    <header className={`header ${className}`}>
        <Logo size="md" showDivider={true} />
        {showThemeToggle && <ThemeToggle />}
    </header>
);

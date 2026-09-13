import { HardDrive, UserCog, Shield, type LucideIcon } from "lucide-react";

export interface NavItem {
    path: string;
    label: string;
    icon: LucideIcon;
    adminOnly?: boolean;
}

export const APP_NAV_ITEMS: NavItem[] = [
    { path: "/drive", label: "Drive", icon: HardDrive },
    { path: "/profile", label: "Profile", icon: UserCog },
    { path: "/admin", label: "Admin", icon: Shield, adminOnly: true },
];

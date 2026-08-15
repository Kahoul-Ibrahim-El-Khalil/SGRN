export type Theme = "light" | "dark" | "jet-black" | "ayu-dark" | "catppuccin" | "dracula" | "nord" | "gruvbox" | "tokyo-night" | "retro";

export interface ThemeOption {
    id: Theme;
    name: string;
    preview: {
        bg: string;
        text: string;
        accent: string;
    };
}

export const THEME_STORAGE_KEY = "theme";

export const THEME_OPTIONS: ThemeOption[] = [
    {
        id: "jet-black",
        name: "Jet Black",
        preview: { bg: "#050505", text: "#F5F5F5", accent: "#F59E0B" },
    },
    {
        id: "dark",
        name: "Dark",
        preview: { bg: "#0E1014", text: "#F3F4F6", accent: "#F59E0B" },
    },
    {
        id: "light",
        name: "Light",
        preview: { bg: "#F2F3F5", text: "#111822", accent: "#D97706" },
    },
    {
        id: "ayu-dark",
        name: "Ayu Dark",
        preview: { bg: "#0F1419", text: "#E6E6E6", accent: "#FFB554" },
    },
    {
        id: "catppuccin",
        name: "Catppuccin",
        preview: { bg: "#1E1E2E", text: "#CDD6F4", accent: "#CBA6F7" },
    },
    {
        id: "dracula",
        name: "Dracula",
        preview: { bg: "#282A36", text: "#F8F8F2", accent: "#FF79C6" },
    },
    {
        id: "nord",
        name: "Nord",
        preview: { bg: "#2E3440", text: "#ECEFF4", accent: "#88C0D0" },
    },
    {
        id: "gruvbox",
        name: "Gruvbox",
        preview: { bg: "#282828", text: "#EBDBB2", accent: "#FABD2F" },
    },
    {
        id: "tokyo-night",
        name: "Tokyo Night",
        preview: { bg: "#1A1B26", text: "#C0CAF5", accent: "#7AA2F7" },
    },
    {
        id: "retro",
        name: "Retro",
        preview: { bg: "#0D0208", text: "#00FF41", accent: "#00FF41" },
    },
];

const DEFAULT_THEME: Theme = "jet-black";

export function isTheme(value: string | null): value is Theme {
    if (!value) return false;
    return THEME_OPTIONS.some((t) => t.id === value);
}

export function getStoredTheme(): Theme {
    const saved = localStorage.getItem(THEME_STORAGE_KEY);
    return isTheme(saved) ? saved : DEFAULT_THEME;
}

export function applyTheme(theme: Theme) {
    const root = document.documentElement;

    THEME_OPTIONS.forEach((themeOption) => {
        root.classList.remove(themeOption.id);
    });

    root.classList.add(theme);
    root.dataset.theme = theme;
    localStorage.setItem(THEME_STORAGE_KEY, theme);
}

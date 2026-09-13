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

/** Themes shown in the selector — professional / government-style defaults. */
export const THEME_OPTIONS: ThemeOption[] = [
    {
        id: "light",
        name: "Light",
        preview: { bg: "#e8eaec", text: "#111314", accent: "#006a96" },
    },
    {
        id: "dark",
        name: "Dark",
        preview: { bg: "#1c1c1c", text: "#dcdcdc", accent: "#0090c4" },
    },
    {
        id: "jet-black",
        name: "Slate",
        preview: { bg: "#0f172a", text: "#e2e8f0", accent: "#0090c4" },
    },
];

/** Every theme class ever emitted — used to strip old classes on switch. */
export const ALL_THEME_IDS: Theme[] = [
    "light",
    "dark",
    "jet-black",
    "ayu-dark",
    "catppuccin",
    "dracula",
    "nord",
    "gruvbox",
    "tokyo-night",
    "retro",
];

const DEFAULT_THEME: Theme = "light";

const SELECTABLE_THEMES: Theme[] = THEME_OPTIONS.map((option) => option.id);

export function isTheme(value: string | null): value is Theme {
    if (!value) return false;
    return ALL_THEME_IDS.includes(value as Theme);
}

export function getStoredTheme(): Theme {
    const saved = localStorage.getItem(THEME_STORAGE_KEY);
    if (isTheme(saved) && SELECTABLE_THEMES.includes(saved)) {
        return saved;
    }
    return DEFAULT_THEME;
}

export function applyTheme(theme: Theme) {
    const root = document.documentElement;

    ALL_THEME_IDS.forEach((id) => {
        root.classList.remove(id);
    });

    root.classList.add(theme);
    root.dataset.theme = theme;
    root.style.colorScheme = theme === "light" ? "light" : "dark";
    localStorage.setItem(THEME_STORAGE_KEY, theme);
}

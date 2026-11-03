// Application URL prefix shared by the router and every hard navigation.
//
// The dashboard is deployed behind nginx under a sub-path (see
// sgrn/lib/datastore/configs/nginx/sites-enabled/sgrn.conf, which strips
// `/datastore` and proxies to the embedded UI). React Router must know that
// prefix as its `basename`, otherwise a reload at /datastore/admin makes the
// router see the undecorated path, match nothing, and bounce to signin.
//
// Baked at build time from VITE_BASE_PATH (the CMake dashboard build passes
// VITE_BASE_PATH=/datastore explicitly); defaults to /datastore so a plain
// `bun run build` matches the shipped nginx contract. NOTE: `vite dev` is
// affected by this default — the router would mount outside /datastore and
// render nothing at the server root. That is why vite.config.ts defines
// VITE_BASE_PATH="/" in serve mode. `bun run dev` starts it via dev.ts.

declare global {
    interface ImportMetaEnv {
        readonly VITE_BASE_PATH?: string;
    }
}

function normalize(t_raw: string | undefined): string {
    if (!t_raw || t_raw === "/") return "/";
    const trimmed = `/${t_raw}/`.replace(/\/{2,}/g, "/");
    return trimmed.endsWith("/") && trimmed.length > 1 ? trimmed.slice(0, -1) : trimmed;
}

export function getBasename(): string {
    return normalize(import.meta.env.VITE_BASE_PATH ?? "/datastore");
}

// Prefix a root-absolute app path ("'/signin'" -> "'/datastore/signin'").
// react-router's navigate()/Link already apply the basename — use this only
// for raw window.location navigations that bypass the router.
export function appPath(t_path: string): string {
    const base = getBasename();
    const path = t_path.startsWith("/") ? t_path : `/${t_path}`;
    return base === "/" ? path : `${base}${path}`;
}

// useWorkingCopy — an editable draft of a server document.
//
// The classic pairing is `[doc, setDoc]` + `[draft, setDraft]` plus a `load()`
// that must remember to reset the draft after every fetch. Here the draft is
// derived from its source: whenever the source *identity* changes (a reload
// produced a new object) the draft is re-cloned from it, in the same render,
// so the UI never shows a stale draft against fresh data.
import { useState, type Dispatch, type SetStateAction } from "react";

interface Copy<T> {
    source: T;
    draft: T;
}

export function useWorkingCopy<T>(
    t_source: T,
    t_clone: (t_value: T) => T = (v) => structuredClone(v),
): [T, Dispatch<SetStateAction<T>>, () => void] {
    const [copy, setCopy] = useState<Copy<T>>(() => ({ source: t_source, draft: t_clone(t_source) }));

    let current = copy;
    if (copy.source !== t_source) {
        // Derived-state update during render (React re-renders immediately,
        // before anything is committed or painted).
        current = { source: t_source, draft: t_clone(t_source) };
        setCopy(current);
    }

    const setDraft: Dispatch<SetStateAction<T>> = (t_next) =>
        setCopy((prev) => {
            const base = prev.source === t_source ? prev : { source: t_source, draft: t_clone(t_source) };
            return { source: base.source, draft: typeof t_next === "function" ? (t_next as (t_prev: T) => T)(base.draft) : t_next };
        });
    const discard = () => setCopy({ source: t_source, draft: t_clone(t_source) });

    return [current.draft, setDraft, discard];
}

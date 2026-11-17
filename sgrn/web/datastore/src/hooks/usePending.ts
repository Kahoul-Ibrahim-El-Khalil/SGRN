// usePending / usePendingKeys — "is this action running?" without a useState
// per button.
//
//   const saving = usePendingKeys<number>();
//   await saving.run(user.id, () => saveUser(user));
//   <button disabled={saving.has(user.id)}>
//
//   const creating = usePending();
//   await creating.run(() => createDomain(body));
//   <button disabled={creating.pending}>
//
// `run` always clears the flag in a finally block, so a thrown error can never
// leave a button stuck in its spinner state.
import { useCallback, useState } from "react";

export interface Pending {
    pending: boolean;
    run: <R>(t_task: () => Promise<R>) => Promise<R>;
}

export function usePending(): Pending {
    // A counter, not a boolean: overlapping runs keep the flag up until the last one ends.
    const [count, setCount] = useState(0);
    const run = useCallback(async <R>(t_task: () => Promise<R>): Promise<R> => {
        setCount((c) => c + 1);
        try {
            return await t_task();
        } finally {
            setCount((c) => c - 1);
        }
    }, []);
    return { pending: count > 0, run };
}

export interface PendingKeys<K extends string | number> {
    /** Keys with an action in flight. */
    keys: ReadonlySet<K>;
    has: (t_key: K) => boolean;
    run: <R>(t_key: K, t_task: () => Promise<R>) => Promise<R>;
}

export function usePendingKeys<K extends string | number>(): PendingKeys<K> {
    const [counts, setCounts] = useState<ReadonlyMap<K, number>>(new Map());

    const bump = (t_key: K, t_delta: 1 | -1) =>
        setCounts((prev) => {
            const next = new Map(prev);
            const value = (next.get(t_key) ?? 0) + t_delta;
            if (value <= 0) next.delete(t_key);
            else next.set(t_key, value);
            return next;
        });

    const run = async <R>(t_key: K, t_task: () => Promise<R>): Promise<R> => {
        bump(t_key, 1);
        try {
            return await t_task();
        } finally {
            bump(t_key, -1);
        }
    };

    return { keys: new Set(counts.keys()), has: (t_key) => counts.has(t_key), run };
}

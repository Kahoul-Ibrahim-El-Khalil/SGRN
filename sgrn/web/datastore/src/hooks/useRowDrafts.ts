// useRowDrafts — per-row edit drafts for table editors.
//
// A draft exists only once the user touches a row; until then the row renders
// straight from the server value, so a reload never fights a stale draft. The
// draft shape is a real type (not `Record<string, string>`), so `patch()` only
// accepts fields the draft actually has.
//
//   const drafts = useRowDrafts<OrgRow, OrgDraft, string>((o) => o.name, orgToDraft);
//   const draft = drafts.draftFor(org);
//   <input value={draft.description} onChange={(e) => drafts.patch(org, { description: e.target.value })} />
//   drafts.clear();   // after a reload
import { useState } from "react";

export interface RowDrafts<Row, Draft> {
    /** The edited draft for `row`, or a fresh one derived from the row. */
    draftFor: (t_row: Row) => Draft;
    /** The draft only if the user has edited this row. */
    peek: (t_row: Row) => Draft | undefined;
    patch: (t_row: Row, t_patch: Partial<Draft>) => void;
    /** Replace a row's draft wholesale (e.g. when opening an editor). */
    begin: (t_row: Row) => void;
    discard: (t_row: Row) => void;
    clear: () => void;
}

export function useRowDrafts<Row, Draft, K extends string | number = number>(
    t_keyOf: (t_row: Row) => K,
    t_makeDraft: (t_row: Row) => Draft,
): RowDrafts<Row, Draft> {
    const [edited, setEdited] = useState<ReadonlyMap<K, Draft>>(new Map());

    return {
        draftFor: (t_row) => edited.get(t_keyOf(t_row)) ?? t_makeDraft(t_row),
        peek: (t_row) => edited.get(t_keyOf(t_row)),
        patch: (t_row, t_patch) =>
            setEdited((prev) => {
                const key = t_keyOf(t_row);
                const next = new Map(prev);
                next.set(key, { ...(prev.get(key) ?? t_makeDraft(t_row)), ...t_patch });
                return next;
            }),
        begin: (t_row) =>
            setEdited((prev) => {
                const next = new Map(prev);
                next.set(t_keyOf(t_row), t_makeDraft(t_row));
                return next;
            }),
        discard: (t_row) =>
            setEdited((prev) => {
                const next = new Map(prev);
                next.delete(t_keyOf(t_row));
                return next;
            }),
        clear: () => setEdited(new Map()),
    };
}

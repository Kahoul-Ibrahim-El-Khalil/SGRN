// useFormState — one typed object (and one reducer) instead of a useState per field.
//
//   const form = useFormState({ name: "", storageMb: "", bootstrapAdmin: false });
//   <input {...form.text("name")} />
//   <input type="checkbox" {...form.checkbox("bootstrapAdmin")} />
//   form.reset();
//
// `text()` only accepts keys whose value is a string and `checkbox()` only keys
// whose value is a boolean, so a typo or a type mismatch is a compile error.
import { useCallback, useReducer, useState, type ChangeEvent } from "react";

type KeysOfType<T, V> = { [K in keyof T]-?: T[K] extends V ? K : never }[keyof T];

type FormAction<T> = { type: "set"; key: keyof T; value: T[keyof T] } | { type: "patch"; patch: Partial<T> } | { type: "reset"; next: T };

function formReducer<T extends object>(t_state: T, t_action: FormAction<T>): T {
    switch (t_action.type) {
        case "set":
            return Object.is(t_state[t_action.key], t_action.value) ? t_state : { ...t_state, [t_action.key]: t_action.value };
        case "patch":
            return { ...t_state, ...t_action.patch };
        case "reset":
            return t_action.next;
    }
}

export interface TextFieldProps {
    value: string;
    onChange: (t_event: ChangeEvent<HTMLInputElement | HTMLSelectElement | HTMLTextAreaElement>) => void;
}

export interface CheckboxFieldProps {
    checked: boolean;
    onChange: (t_event: ChangeEvent<HTMLInputElement>) => void;
}

export interface FormState<T extends object> {
    values: T;
    setField: <K extends keyof T>(t_key: K, t_value: T[K]) => void;
    patch: (t_patch: Partial<T>) => void;
    /** Back to the initial values (or to `t_next` when given). */
    reset: (t_next?: T) => void;
    /** Props for an <input>/<select>/<textarea> bound to a string field. */
    text: <K extends KeysOfType<T, string>>(t_key: K) => TextFieldProps;
    /** Props for a checkbox bound to a boolean field. */
    checkbox: <K extends KeysOfType<T, boolean>>(t_key: K) => CheckboxFieldProps;
}

export function useFormState<T extends object>(t_initial: T): FormState<T> {
    // Captured once so reset() always returns to what the form started as.
    const [initial] = useState(t_initial);
    const [values, dispatch] = useReducer(formReducer<T>, initial);

    const setField = useCallback(<K extends keyof T>(t_key: K, t_value: T[K]) => {
        dispatch({ type: "set", key: t_key, value: t_value });
    }, []);
    const patch = useCallback((t_patch: Partial<T>) => dispatch({ type: "patch", patch: t_patch }), []);
    const reset = useCallback((t_next?: T) => dispatch({ type: "reset", next: t_next ?? initial }), [initial]);

    const text = <K extends KeysOfType<T, string>>(t_key: K): TextFieldProps => ({
        value: values[t_key] as unknown as string,
        onChange: (t_event) => setField(t_key, t_event.target.value as T[K]),
    });
    const checkbox = <K extends KeysOfType<T, boolean>>(t_key: K): CheckboxFieldProps => ({
        checked: values[t_key] as unknown as boolean,
        onChange: (t_event) => setField(t_key, t_event.target.checked as T[K]),
    });

    return { values, setField, patch, reset, text, checkbox };
}

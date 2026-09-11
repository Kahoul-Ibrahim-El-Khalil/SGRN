import { useState } from "react";

export function useAuthForm() {
    const [formData, setFormData] = useState({ email: "", password: "" });
    const handleChange = (e: React.ChangeEvent<HTMLInputElement>) => {
        const { name, value } = e.target;
        setFormData((prev) => ({ ...prev, [name]: value }));
    };
    return { formData, handleChange };
}

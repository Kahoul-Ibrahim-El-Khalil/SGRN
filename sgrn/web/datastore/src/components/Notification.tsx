// @/components/Notification.tsx
import { AlertCircle, CheckCircle, X } from "lucide-react";

interface NotificationProps {
    type: "error" | "success";
    message: string;
    onClose: () => void;
}

export function Notification({ type, message, onClose }: NotificationProps) {
    const isError = type === "error";

    return (
        <div
            className={`p-4 rounded-lg border flex items-center gap-3 ${
                isError
                    ? "bg-red-50 dark:bg-red-900/20 border-red-200 dark:border-red-800"
                    : "bg-green-50 dark:bg-green-900/20 border-green-200 dark:border-green-800"
            }`}
        >
            {isError ? (
                <AlertCircle className="text-red-600 dark:text-red-400 flex-shrink-0" size={20} />
            ) : (
                <CheckCircle className="text-green-600 dark:text-green-400 flex-shrink-0" size={20} />
            )}
            <span
                className={`text-sm font-medium flex-1 ${
                    isError ? "text-red-700 dark:text-red-300" : "text-green-700 dark:text-green-300"
                }`}
            >
                {message}
            </span>
            <button onClick={onClose} className="text-slate-600 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200">
                <X size={18} />
            </button>
        </div>
    );
}

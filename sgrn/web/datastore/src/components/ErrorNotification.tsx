import { X } from "lucide-react";
import { useError } from "@/contexts/ErrorContext";

export function ErrorNotification() {
    const { error, clearError } = useError();

    if (!error) return null;

    return (
        <div className="fixed top-4 right-4 z-[9999] animate-slideIn">
            <div className="bg-red-50 dark:bg-red-900/30 border-l-4 border-red-500 rounded-lg shadow-xl p-4 max-w-md backdrop-blur-sm">
                <div className="flex items-start gap-3">
                    <div className="flex-shrink-0">
                        <svg className="h-5 w-5 text-red-500" viewBox="0 0 20 20" fill="currentColor">
                            <path
                                fillRule="evenodd"
                                d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.707 7.293a1 1 0 00-1.414 1.414L8.586 10l-1.293 1.293a1 1 0 101.414 1.414L10 11.414l1.293 1.293a1 1 0 001.414-1.414L11.414 10l1.293-1.293a1 1 0 00-1.414-1.414L10 8.586 8.707 7.293z"
                                clipRule="evenodd"
                            />
                        </svg>
                    </div>
                    <div className="flex-1 min-w-0">
                        <h3 className="text-sm font-semibold text-red-800 dark:text-red-200">Error</h3>
                        <p className="mt-1 text-sm text-red-700 dark:text-red-300 break-words">{error}</p>
                    </div>
                    <button
                        onClick={clearError}
                        className="flex-shrink-0 text-red-400 hover:text-red-600 dark:hover:text-red-200 transition-colors"
                        aria-label="Close"
                    >
                        <X size={20} />
                    </button>
                </div>
            </div>
            <style>{`
        @keyframes slideIn {
          from {
            transform: translateX(100%);
            opacity: 0;
          }
          to {
            transform: translateX(0);
            opacity: 1;
          }
        }
        .animate-slideIn {
          animation: slideIn 0.3s ease-out;
        }
      `}</style>
        </div>
    );
}

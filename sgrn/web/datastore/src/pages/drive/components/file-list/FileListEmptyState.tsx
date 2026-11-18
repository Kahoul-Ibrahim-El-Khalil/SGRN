import { File } from "lucide-react";

export function FileListEmptyState() {
    return (
        <div className="drive-empty">
            <div className="p-8 border-2 border-dashed border-border/20 rounded-lg flex flex-col items-center gap-4">
                <File size={48} strokeWidth={1} className="text-muted-foreground/20" />
                <p className="text-[10px] font-black uppercase tracking-[0.3em]">Directory is empty</p>
            </div>
        </div>
    );
}

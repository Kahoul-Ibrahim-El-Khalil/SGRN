// @/components/PageLayout.tsx
import { NavigationBar } from "@/components/NavigationBar";
import { Sidebar } from "@/components/Sidebar";

interface PageLayoutProps {
    children: React.ReactNode;
    className?: string;
    title?: string;
}

export const PageLayout = ({ children: t_children, className: t_class_name = "", title }: PageLayoutProps) => {
    return (
        <div className="desktop-shell">
            <Sidebar />

            <main className="desktop-main">
                <NavigationBar />

                <div className={`desktop-content ${t_class_name}`}>
                    {title && (
                        <div className="page-header">
                            <h1 className="page-title">{title}</h1>
                            <div className="page-header-line" />
                        </div>
                    )}
                    {t_children}
                </div>
            </main>
        </div>
    );
};

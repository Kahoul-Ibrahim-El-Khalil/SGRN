// @/components/Logos.tsx
import React from "react";
import ENSTI_LOGO from "@/assets/logos/ENSTI.png";
import ENSTI_GI_DEPARTEMENT_LOGO from "@/assets/logos/GI.png";

const ENSTI_WEBSITE_URL: string = "https://ensti-annaba.dz/";
const ENSTI_GI_URL: string = "https://ensti-annaba.dz/departement-gi/";

export const ENSTILogosHeader: React.FC = () => (
    <header className="flex w-full items-center justify-center md:justify-start gap-6 select-none">
        {/* ENSTI Main Logo */}
        <div
            onClick={() => window.open(ENSTI_WEBSITE_URL, "_blank")}
            title="Visit ENSTI website"
            className="cursor-pointer transition-transform duration-300 hover:scale-105 active:scale-95"
        >
            <img
                src={ENSTI_LOGO}
                alt="ENSTI Logo"
                // Mobile: 16 (64px) | Desktop: 20 (80px)
                className="h-16 w-16 md:h-20 md:w-20 object-contain"
            />
        </div>

        {/* Optional: Vertical Divider for aesthetics */}
        <div className="h-10 w-[1px] bg-slate-300 dark:bg-slate-700 opacity-50" />

        {/* GI Department Logo */}
        <div
            onClick={() => window.open(ENSTI_GI_URL, "_blank")}
            title="Visit ENSTI GI Department page"
            className="cursor-pointer transition-transform duration-300 hover:scale-105 active:scale-95"
        >
            <img src={ENSTI_GI_DEPARTEMENT_LOGO} alt="ENSTI GI Department Logo" className="h-16 w-16 md:h-20 md:w-20 object-contain" />
        </div>
    </header>
);

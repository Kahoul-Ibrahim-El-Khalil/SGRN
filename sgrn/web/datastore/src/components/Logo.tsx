// @/components/Logo.tsx
import React from "react";
import ENSTI_LOGO from "@/assets/logos/ENSTI.png";
import ENSTI_GI_DEPARTEMENT_LOGO from "@/assets/logos/GI.png";

const ENSTI_WEBSITE_URL: string = "https://ensti-annaba.dz/";
const ENSTI_GI_URL: string = "https://ensti-annaba.dz/departement-gi/";

interface LogoProps {
    size?: "sm" | "md" | "lg";
    showDivider?: boolean;
}

export const Logo: React.FC<LogoProps> = ({ size = "md", showDivider = true }) => (
    <div className={`logo-container logo-container--${size}`}>
        {/* ENSTI Main Logo */}
        <div onClick={() => window.open(ENSTI_WEBSITE_URL, "_blank")} title="Visit ENSTI website" className="logo-item">
            <img src={ENSTI_LOGO} alt="ENSTI Logo" className="logo-img" />
        </div>

        {/* Optional: Vertical Divider for aesthetics */}
        {showDivider && <div className="logo-divider" />}

        {/* GI Department Logo */}
        <div onClick={() => window.open(ENSTI_GI_URL, "_blank")} title="Visit ENSTI GI Department page" className="logo-item">
            <img src={ENSTI_GI_DEPARTEMENT_LOGO} alt="ENSTI GI Department Logo" className="logo-img" />
        </div>
    </div>
);

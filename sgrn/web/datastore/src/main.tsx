// @/main.tsx
import React from "react";
import ReactDOM from "react-dom/client";
import { BrowserRouter } from "react-router-dom";
import App from "./App";
import { getBasename } from "@/lib/appPath";
import "./index.css";

ReactDOM.createRoot(document.getElementById("root")!).render(
    <React.StrictMode>
        <BrowserRouter basename={getBasename()}>
            <App />
        </BrowserRouter>
    </React.StrictMode>,
);

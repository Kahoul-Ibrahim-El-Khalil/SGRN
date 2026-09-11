// src/vite-env.d.ts
/// <reference types="vite/client" />

declare module "*?worker" {
    const workerConstructor: {
        new (): Worker;
    };
    export default workerConstructor;
}

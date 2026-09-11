declare module "fs" {
  export function readdirSync(...args: any[]): any;
  export function statSync(...args: any[]): any;
}

declare module "path" {
  export function join(...args: any[]): string;
  export function resolve(...args: any[]): string;
}

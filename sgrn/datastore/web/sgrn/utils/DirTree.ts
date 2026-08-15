import { ErrorScope, SgrnResult } from "@sgrn/types";
import { readdirSync, statSync } from "fs";
import { join, resolve } from "path";

// ── Node ─────────────────────────────────────────────────────────────────────

type NodeType = "file" | "dir";

export class TreeNode {
    name: string;
    /** Relative path from the tree root (traversing root→leaf reconstructs it). */
    path: string;
    /** Absolute path on disk — use this for reading/uploading the file. */
    absolute_path: string;
    type: NodeType;
    depth: number;
    children: TreeNode[];

    constructor(t_name: string, t_path: string, t_absolute_path: string, t_type: NodeType, t_depth: number) {
        this.name = t_name;
        this.path = t_path;
        this.absolute_path = t_absolute_path;
        this.type = t_type;
        this.depth = t_depth;
        this.children = [];
    }

    isFile(): boolean {
        return this.type === "file";
    }

    isDir(): boolean {
        return this.type === "dir";
    }
}

// ── Tree ─────────────────────────────────────────────────────────────────────

export class DirectoryTree {
    /** The head node — represents the root path (file or directory). */
    head: TreeNode;

    /** Maximum depth to traverse. null means unlimited. */
    max_depth: number | null;

    private constructor(t_head: TreeNode, t_max_depth: number | null = null) {
        this.head = t_head;
        this.max_depth = t_max_depth;
    }

    /**
     * Static factory to build a tree from a root path.
     */
    public static build(t_root_path: string, t_max_depth: number | null = null): SgrnResult<DirectoryTree> {
        try {
            const absolute_path = resolve(t_root_path);
            const stat = statSync(absolute_path);
            const type: NodeType = stat.isDirectory() ? "dir" : "file";

            const head = new TreeNode(absolute_path, absolute_path, absolute_path, type, 0);
            const tree = new DirectoryTree(head, t_max_depth);

            if (type === "dir") {
                const res = tree.#populate(head, absolute_path);
                if (res.error) return res as SgrnResult<any>;
            }

            return { data: tree };
        } catch (e) {
            return { error: `Failed to build directory tree: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.FileSystem };
        }
    }

    // Recursively populate children from the filesystem.
    #populate(t_node: TreeNode, t_absolute_path: string): SgrnResult<void> {
        if (this.max_depth !== null && t_node.depth >= this.max_depth) return { data: undefined };

        try {
            const entries = readdirSync(t_absolute_path, { withFileTypes: true });

            for (const entry of entries) {
                const child_absolute = join(t_absolute_path, entry.name);

                const child_relative = t_node.depth === 0 ? entry.name : `${t_node.path}/${entry.name}`;

                const type: NodeType = entry.isDirectory() ? "dir" : "file";
                const child = new TreeNode(entry.name, child_relative, child_absolute, type, t_node.depth + 1);

                t_node.children.push(child);

                if (type === "dir") {
                    const res = this.#populate(child, child_absolute);
                    if (res.error) return res;
                }
            }
            return { data: undefined };
        } catch (e) {
            return {
                error: `Failed to read directory ${t_absolute_path}: ${e instanceof Error ? e.message : String(e)}`,
                scope: ErrorScope.FileSystem,
            };
        }
    }

    /**
     * Traverse the entire tree starting from `head`.
     * `fn` is called on every node in pre-order (parent before children).
     * Return `false` from `fn` to stop traversal early.
     */
    traverse(t_fn: (t_node: TreeNode) => void | false): void {
        this.#visit(this.head, t_fn);
    }

    #visit(t_node: TreeNode, t_fn: (t_n: TreeNode) => void | false): boolean {
        const result = t_fn(t_node);
        if (result === false) {
            return false;
        }
        for (const child of t_node.children) {
            if (!this.#visit(child, t_fn)) return false;
        }

        return true;
    }
}

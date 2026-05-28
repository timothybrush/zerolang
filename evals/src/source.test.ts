import assert from "node:assert/strict";
import { describe, it } from "node:test";
import {
  extractZeroSource,
  finalSourceResponseFailures,
  sourcePatternFailures,
} from "./source.js";

describe("eval source helpers", () => {
  it("extracts fenced Zero code", () => {
    const source = extractZeroSource("```zero\npub fn main() -> Void {}\n```");
    assert.equal(source, "pub fn main() -> Void {}\n");
  });

  it("keeps plain source", () => {
    const source = extractZeroSource("pub fn main() -> Void {}");
    assert.equal(source, "pub fn main() -> Void {}\n");
  });

  it("reports missing source patterns", () => {
    assert.deepEqual(sourcePatternFailures("hello", [/hello/, /zero/]), [
      "/zero/",
    ]);
  });

  it("accepts a source-only final response", () => {
    assert.deepEqual(
      finalSourceResponseFailures("pub fn main() -> Void {}\n", "pub fn main() -> Void {}\n"),
      [],
    );
  });

  it("rejects prose or Markdown around final source", () => {
    assert.deepEqual(
      finalSourceResponseFailures(
        "Here is the source:\n\n```zero\npub fn main() -> Void {}\n```",
        "pub fn main() -> Void {}\n",
      ),
      ["final response included prose or Markdown around the source"],
    );
  });

  it("rejects unfenced prose before source", () => {
    assert.deepEqual(
      finalSourceResponseFailures(
        "The program checks cleanly.\n\npub fn main() -> Void {}",
        "The program checks cleanly.\n\npub fn main() -> Void {}\n",
      ),
      ["final response did not start with Zero source"],
    );
  });
});

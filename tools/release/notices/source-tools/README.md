# Source tooling references

These notices accompany source-side development/release tools. They are separate from the dependencies compiled into or loaded by Single Editor. They do not select the license for Single Editor's own code.

## Microsoft PDB / CodeView

[`../../pdb/InlineAnnotations.mjs`](../../pdb/InlineAnnotations.mjs) names `BinaryAnnotationOpcode`, `CVUncompressData` and `DecodeSignedInt32` from Microsoft's `include/cvinfo.h`. The JavaScript code reimplements the CodeView compressed integers, signed values and the observed annotation operations; it adds local input/range checks. It does not copy the entire header or original C++ function bodies. We retain the upstream copyright and complete [MIT license](Microsoft-PDB-LICENSE.txt) with the source rather than treating the algorithm reference as irrelevant to provenance.

- Upstream repository: [Microsoft microsoft-pdb](https://github.com/microsoft/microsoft-pdb).
- Fixed source reference: [cvinfo.h at 805655a28bd8198004be2ac27e6e0290121a5e89](https://github.com/microsoft/microsoft-pdb/blob/805655a28bd8198004be2ac27e6e0290121a5e89/include/cvinfo.h).
- Exact notice source: [LICENSE at the same commit](https://raw.githubusercontent.com/microsoft/microsoft-pdb/805655a28bd8198004be2ac27e6e0290121a5e89/LICENSE).
- Original notice: 1157 bytes, SHA-256 `4C21DC82A1C7186EA754FD59D504C00947953A3401FE8E24EAA44109EF8E2555`.
- Scope: public source for the build-side annotation decoder. This does not add a Microsoft PDB runtime DLL or a new product build dependency.

The annotation implementation and its existing evidence identities are unchanged by this notice preparation. Keep this directory when delivering the referenced source tool; it is not added to the GUI/CLI runtime payload merely because the source review tool uses it.

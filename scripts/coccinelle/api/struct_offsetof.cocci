// SPDX-License-Identifier: GPL-2.0-only
/// Use struct_offsetof() family of macros
///
// Confidence: High
@@
expression ptr, member;
@@
- offsetof(typeof(*ptr), member)
+ struct_offset(ptr, member)
@@
expression ptr, member;
@@
- offsetofend(typeof(*ptr), member)
+ struct_offsetofend(ptr, member)

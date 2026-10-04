# Disposable SEGW service cleanup

Use this checklist only for a disposable project created for a test. Before generating runtime artifacts, record the exact SEGW project, technical service, technical model, frontend service, ICF leaf, system alias assignment, and generated class names. SEGW's runtime artifact list is the authority for the generated names; do not infer them from a naming pattern.

The order below was established while removing `ZFFLY260930_SRV`. Deleting the SEGW project alone left the registered service, model, and four generated classes behind. The frontend service could not be deleted until its ICF leaf and `LOCAL` alias assignment were removed.

On 2026-09-26 the same order was completed through Fairyfly GUI controls for `ZFFLY260926B_SRV`. Its generated names were recorded from SEGW's Model and Service Definition dialog and Runtime Artifacts tree. Fresh SICF, Gateway, SEGW, and SE24 checks found every disposable layer absent afterward.

On 2026-09-27, the full checklist was repeated through Fairyfly CLI GridView row selection and toolbar actions for `ZFFLY260927D_SRV`. SICF displayed this leaf by alternate name `zffly260927d_srv`, while its service detail showed original ICF name `A4H001000000013`, created on the same day by `DEVELOPER`. The delete confirmation used the original name. Verify this original-to-alternate-name mapping in the detail screen before confirming an ICF deletion when the prompt differs from the visible tree label. After cleanup, the SICF leaf, frontend service and alias, backend service and model, SEGW project, and all four classes were independently absent. The saved GUI connection and session were closed.

1. In SICF, locate the exact leaf under `/sap/opu/odata/sap/<frontend-service>` and delete that leaf. Verify the leaf is absent; leave its parent nodes and other services intact.
2. In `/IWFND/MAINT_SERVICE`, filter to the exact frontend service and confirm its technical name before selecting the row. In its **System Aliases** grid, unassign only the alias recorded for this test. Delete the selected frontend service, then verify it no longer appears in the service list.
3. In `/IWBEP/REG_SERVICE`, delete the exact backend technical service recorded for the project. Verify it is absent.
4. In `/IWBEP/REG_MODEL`, delete the exact backend technical model recorded for the project. Verify it is absent.
5. In SEGW, delete the disposable project and confirm its tree node is gone.
6. In SE24, delete each class listed in the project's generated runtime artifacts. Delete extension subclasses (`DPC_EXT`, `MPC_EXT`) before their base classes (`DPC`, `MPC`); deleting a base first prompts that its subclass will be invalidated. Check each exact class name afterward; SE24 should report that it does not exist.

If a deletion is refused, keep the remaining identifiers in the test record and resolve the stated prerequisite before continuing. Verify each layer independently; an unavailable OData URL alone does not prove that backend registrations or classes were removed.

/**
 *	
 * $Id$ 
 *
 * ***** BEGIN GPL/BL DUAL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. The Blender
 * Foundation also sells licenses for use in proprietary software under
 * the Blender License.  See http://www.blender.org/BL/ for information
 * about this.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL/BL DUAL LICENSE BLOCK *****
 */
#ifndef BKE_MODIFIER_H
#define BKE_MODIFIER_H

struct EditMesh;
struct DerivedMesh;
struct DagForest;
struct DagNode;
struct Object;
struct ListBase;
struct bArmature;

typedef enum {
	/* Should not be used, only for None modifier type */
	eModifierTypeType_None,

	/* Modifier only does deformation, implies that modifier
	 * type should have a valid deformVerts function. OnlyDeform
	 * style modifiers implicitly accept either mesh or CV
	 * input but should still declare flags appropriately.
	 */
	eModifierTypeType_OnlyDeform,

	eModifierTypeType_Constructive,
	eModifierTypeType_Nonconstructive,
} ModifierTypeType;

typedef enum {
	eModifierTypeFlag_AcceptsMesh          = (1<<0),
	eModifierTypeFlag_AcceptsCVs           = (1<<1),
	eModifierTypeFlag_SupportsMapping      = (1<<2),
	eModifierTypeFlag_SupportsEditmode     = (1<<3),

	/* For modifiers that support editmode this determines if the
	 * modifier should be enabled by default in editmode. This should
	 * only be used by modifiers that are relatively speedy and
	 * also generally used in editmode, otherwise let the user enable
	 * it by hand.
	 */
	eModifierTypeFlag_EnableInEditmode     = (1<<4),

	/* For modifiers that require original data and so cannot
	 * be placed after any non-deformative modifier.
	 */
	eModifierTypeFlag_RequiresOriginalData = (1<<5),
} ModifierTypeFlag;

typedef void (*ObjectWalkFunc)(void *userData, Object *ob, Object **obpoin);
typedef void (*IDWalkFunc)(void *userData, Object *ob, ID **idpoin);

typedef struct ModifierTypeInfo {
	/* The user visible name for this modifier */
	char name[32];

	/* The DNA struct name for the modifier data type, used to
	 * write the DNA data out.
	 */
	char structName[32];

	/* The size of the modifier data type, used by allocation. */
	int structSize;

	ModifierTypeType type;
	ModifierTypeFlag flags;


	/********************* Non-optional functions *********************/

	/* Copy instance data for this modifier type. Should copy all user
	 * level settings to the target modifier.
	 */
	void (*copyData)(ModifierData *md, ModifierData *target);


	/********************* Deform modifier functions *********************/

	/* Only for deform types, should apply the deformation
	 * to the given vertex array. If the deformer requires information from
	 * the object it can obtain it from the derivedData argument if non-NULL,
	 * and otherwise the ob argument.
	 */
	void (*deformVerts)(ModifierData *md, struct Object *ob,
	                    struct DerivedMesh *derivedData,
	                    float (*vertexCos)[3], int numVerts);

	/* Like deformVerts but called during editmode (for supporting modifiers)
	 */
	void (*deformVertsEM)(
	            ModifierData *md, struct Object *ob,
	            struct EditMesh *editData, struct DerivedMesh *derivedData,
	            float (*vertexCos)[3], int numVerts);


	/********************* Non-deform modifier functions *********************/

	/* For non-deform types: apply the modifier and return a derived
	 * data object (type is dependent on object type).
	 *
	 * The derivedData argument should always be non-NULL; the modifier
	 * should read the object data from the derived object instead of the
	 * actual object data. 
	 *
	 * The useRenderParams argument indicates if the modifier is being
	 * applied in the service of the renderer which may alter quality
	 * settings.
	 *
	 * The isFinalCalc parameter indicates if the modifier is being
	 * calculated for a final result or for something temporary
	 * (like orcos). This is a hack at the moment, it is meant so subsurf
	 * can know if it is safe to reuse its internal cache.
	 *
	 * The modifier may reuse the derivedData argument (i.e. return it in
	 * modified form), but must not release it.
	 */
	struct DerivedMesh *(*applyModifier)(
	                            ModifierData *md, struct Object *ob,
	                            struct DerivedMesh *derivedData,
	                            int useRenderParams, int isFinalCalc);

	/* Like applyModifier but called during editmode (for supporting
	 * modifiers).
	 * 
	 * The derived object that is returned must support the operations that
	 * are expected from editmode objects. The same qualifications regarding
	 * derivedData apply as for applyModifier.
	 */
	struct DerivedMesh *(*applyModifierEM)(
	                            ModifierData *md, struct Object *ob,
	                            struct EditMesh *editData,
	                            struct DerivedMesh *derivedData);


	/********************* Optional functions *********************/

	/* Initialize new instance data for this modifier type, this function
	 * should set modifier variables to their default values.
	 * 
	 * This function is optional.
	 */
	void (*initData)(ModifierData *md);

	/* Free internal modifier data variables, this function should
	 * not free the md variable itself.
	 *
	 * This function is optional.
	 */
	void (*freeData)(ModifierData *md);

	/* Return a boolean value indicating if this modifier is able to be
	 * calculated based on the modifier data. This is *not* regarding the
	 * md->flag, that is tested by the system, this is just if the data
	 * validates (for example, a lattice will return false if the lattice
	 * object is not defined).
	 *
	 * This function is optional (assumes never disabled if not present).
	 */
	int (*isDisabled)(ModifierData *md);

	/* Add the appropriate relations to the DEP graph depending on the
	 * modifier data. 
	 *
	 * This function is optional.
	 */
	void (*updateDepgraph)(ModifierData *md, struct DagForest *forest,
	                       struct Object *ob, struct DagNode *obNode);

	/* Should return true if the modifier needs to be recalculated on time
	 * changes.
	 *
	 * This function is optional (assumes false if not present).
	 */
	int (*dependsOnTime)(ModifierData *md);

	/* Should call the given walk function on with a pointer to each Object
	 * pointer that the modifier data stores. This is used for linking on file
	 * load and for unlinking objects or forwarding object references.
	 *
	 * This function is optional.
	 */
	void (*foreachObjectLink)(ModifierData *md, struct Object *ob,
	                          ObjectWalkFunc walk, void *userData);

	/* Should call the given walk function with a pointer to each ID
	 * pointer (i.e. each datablock pointer) that the modifier data
	 * stores. This is used for linking on file load and for
	 * unlinking datablocks or forwarding datablock references.
	 *
	 * This function is optional. If it is not present, foreachObjectLink
	 * will be used.
	 */
	void (*foreachIDLink)(ModifierData *md, struct Object *ob,
	                      IDWalkFunc walk, void *userData);
} ModifierTypeInfo;

ModifierTypeInfo *modifierType_getInfo (ModifierType type);

/* Modifier utility calls, do call through type pointer and return
 * default values if pointer is optional.
 */
ModifierData  *modifier_new(int type);
void          modifier_free(ModifierData *md);

void          modifier_copyData(ModifierData *md, ModifierData *target);
int           modifier_dependsOnTime(ModifierData *md);
int           modifier_supportsMapping(ModifierData *md);
int           modifier_couldBeCage(ModifierData *md);
void          modifier_setError(ModifierData *md, char *format, ...);

void          modifiers_foreachObjectLink(struct Object *ob,
                                          ObjectWalkFunc walk,
                                          void *userData);
void          modifiers_foreachIDLink(struct Object *ob,
                                      IDWalkFunc walk,
                                      void *userData);
ModifierData  *modifiers_findByType(struct Object *ob, ModifierType type);
void          modifiers_clearErrors(struct Object *ob);
int           modifiers_getCageIndex(struct Object *ob,
                                     int *lastPossibleCageIndex_r);

int           modifiers_isSoftbodyEnabled(struct Object *ob);
struct Object *modifiers_isDeformedByArmature(struct Object *ob);
int           modifiers_usesArmature(struct Object *ob, struct bArmature *arm);
int           modifiers_isDeformed(struct Object *ob);

ModifierData  *modifiers_getVirtualModifierList(struct Object *ob);

#endif


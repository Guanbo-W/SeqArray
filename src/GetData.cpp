// ===========================================================
//
// GetData.cpp: Get data from a SeqArray GDS file
//
// Copyright (C) 2015-2020    Xiuwen Zheng
//
// This file is part of SeqArray.
//
// SeqArray is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 3 as
// published by the Free Software Foundation.
//
// SeqArray is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SeqArray.
// If not, see <http://www.gnu.org/licenses/>.

#include "Index.h"
#include "ReadByVariant.h"
#include "ReadBySample.h"


namespace SeqArray
{

// the mode of R data allowing sparse matrix, defined in >= gdsfmt_1.23.6
#ifndef GDS_R_READ_ALLOW_SP_MATRIX
#   define GDS_R_READ_ALLOW_SP_MATRIX    0
#endif

// check macro
#define CHECK_SAMPLE_DIMENSION    \
	if ((vm.NDim != 1) || (vm.Dim[0] != File.SampleNum()))    \
		throw ErrSeqArray(ERR_DIM, vm.Name.c_str());
#define CHECK_VARIANT_ONE_DIMENSION    \
	if ((vm.NDim != 1) || (vm.Dim[0] != File.VariantNum()))    \
		throw ErrSeqArray(ERR_DIM, vm.Name.c_str());


// error information
static const char *ERR_DIM = "Invalid dimension of '%s'.";

// variable list
static const string VAR_GENO("genotype");
static const string VAR_SAMP_ID("sample.id");
static const string VAR_POSITION("position");
static const string VAR_CHROM("chromosome");
static const string VAR_ID("variant.id");
static const string VAR_ALLELE("allele");
static const string VAR_ANNOT_ID("annotation/id");
static const string VAR_ANNOT_QUAL("annotation/qual");
static const string VAR_ANNOT_FILTER("annotation/filter");
static const string VAR_GENOTYPE("genotype");
static const string VAR_GENO_INDEX("@genotype");
static const string VAR_PHASE("phase");

// variable list: internally generated
static const string VAR_DOSAGE("$dosage");
static const string VAR_DOSAGE_ALT("$dosage_alt");
static const string VAR_NUM_ALLELE("$num_allele");
static const string VAR_REF_ALLELE("$ref");
static const string VAR_ALT_ALLELE("$alt");
static const string VAR_CHROM_POS("$chrom_pos");
static const string VAR_CHROM_POS2("$chrom_pos");
static const string VAR_CHROM_POS_ALLELE("$chrom_pos_allele");
static const string VAR_SAMPLE_INDEX("$sample_index");
static const string VAR_VARIANT_INDEX("$variant_index");


/// the parameter structure in TVarMap::TFunction
struct COREARRAY_DLL_LOCAL TParam
{
	int use_raw;
	int padNA;
	int tolist;
	SEXP Env;
	/// constructor
	TParam(int _useraw, int _padNA, int _tolist, SEXP _Env)
	{
		use_raw = _useraw;
		padNA = _padNA;
		tolist = _tolist;
		Env = _Env;
	}
};


// ===========================================================
// Get data from a working space
// ===========================================================

/// get sample id from 'sample.id'
static SEXP get_sample_1d(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	C_BOOL *ss = File.Selection().pSample;
	return GDS_R_Array_Read(Var.Obj, NULL, NULL, &ss,
		GDS_R_READ_DEFAULT_MODE | (P->use_raw ? GDS_R_READ_ALLOW_RAW_TYPE : 0));
}

/// get position from 'position'
static SEXP get_position(CFileInfo &File, TVarMap &Var, void *param)
{
	int n = File.VariantSelNum();
	SEXP rv_ans = NEW_INTEGER(n);
	if (n > 0)
	{
		TSelection &Sel = File.Selection();
		const int *base = &File.Position()[0] + Sel.varStart;
		C_BOOL *s = Sel.pVariant + Sel.varStart;
		for (int *p=INTEGER(rv_ans); n > 0; base++)
			if (*s++) { *p++ = *base; n--; }
	}
	return rv_ans;
}

/// get chromosome from 'chromosome'
static SEXP get_chrom(CFileInfo &File, TVarMap &Var, void *param)
{
	int n = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_CHARACTER(n));
	if (n > 0)
	{
		CChromIndex &Chrom = File.Chromosome();
		TSelection &Sel = File.Selection();
		C_BOOL *s = Sel.pVariant + Sel.varStart;
		size_t p = 0, i = Sel.varStart;
		SEXP lastR = mkChar("");
		string lastS;
		for (; n > 0; i++)
		{
			if (*s++)
			{
				const string &ss = Chrom[i];
				if (ss != lastS)
				{
					lastS = ss;
					lastR = mkChar(ss.c_str());
				}
				SET_STRING_ELT(rv_ans, p++, lastR);
				n--;
			}
		}
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get variant-specific data from node without indexing (e.g., variant.id)
static SEXP get_data_1d(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	TSelection &Sel = File.Selection();
	Sel.GetStructVariant();
	C_BOOL *ss = Sel.pVariant + Sel.varStart;
	C_Int32 dimst  = Sel.varStart;
	C_Int32 dimcnt = Sel.varEnd - Sel.varStart;
	return GDS_R_Array_Read(Var.Obj, &dimst, &dimcnt, &ss,
		GDS_R_READ_DEFAULT_MODE | (P->use_raw ? GDS_R_READ_ALLOW_RAW_TYPE : 0));
}

/// get genotypes from 'genotype/data'
static SEXP get_genotype(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	SEXP rv_ans = R_NilValue;
	const int nSample  = File.SampleSelNum();
	const int nVariant = File.VariantSelNum();
	if ((nSample > 0) && (nVariant > 0))
	{
		// initialize GDS genotype Node
		CApply_Variant_Geno NodeVar(File, P->use_raw);
		// size to be allocated
		ssize_t SIZE = (ssize_t)nSample * File.Ploidy();
		if (P->use_raw)
		{
			rv_ans = PROTECT(NEW_RAW(nVariant * SIZE));
			C_UInt8 *base = (C_UInt8 *)RAW(rv_ans);
			do {
				NodeVar.ReadGenoData(base);
				base += SIZE;
			} while (NodeVar.Next());
		} else {
			rv_ans = PROTECT(NEW_INTEGER(nVariant * SIZE));
			int *base = INTEGER(rv_ans);
			do {
				NodeVar.ReadGenoData(base);
				base += SIZE;
			} while (NodeVar.Next());
		}
		// return R object
		SEXP dim = PROTECT(NEW_INTEGER(3));
		int *p = INTEGER(dim);
		p[0] = File.Ploidy(); p[1] = nSample; p[2] = nVariant;
		SET_DIM(rv_ans, dim);
		SET_DIMNAMES(rv_ans, R_Geno_Dim3_Name);
		UNPROTECT(2);
	}
	// output
	return rv_ans;
}

/// get phasing status from 'phase/data', TODO: optimize
static SEXP get_phase(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	TSelection &Sel = File.Selection();
	C_BOOL *ss[3] = { Sel.pVariant, Sel.pSample, NULL };
	if (Var.NDim == 3)
		ss[2] = NeedArrayTRUEs(Var.Dim[2]);
	return GDS_R_Array_Read(Var.Obj, NULL, NULL, ss,
		GDS_R_READ_DEFAULT_MODE | GDS_R_READ_ALLOW_SP_MATRIX |
		(P->use_raw ? GDS_R_READ_ALLOW_RAW_TYPE : 0));
}

/// get dosage of reference allele from 'genotype/data'
static SEXP get_dosage(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	SEXP rv_ans = R_NilValue;
	ssize_t nSample  = File.SampleSelNum();
	ssize_t nVariant = File.VariantSelNum();
	if ((nSample > 0) && (nVariant > 0))
	{
		// initialize GDS genotype Node
		CApply_Variant_Dosage NodeVar(File, false, false);
		if (P->use_raw)
		{
			rv_ans = PROTECT(allocMatrix(RAWSXP, nSample, nVariant));
			C_UInt8 *base = (C_UInt8 *)RAW(rv_ans);
			do {
				NodeVar.ReadDosage(base);
				base += nSample;
			} while (NodeVar.Next());
		} else {
			rv_ans = PROTECT(allocMatrix(INTSXP, nSample, nVariant));
			int *base = INTEGER(rv_ans);
			do {
				NodeVar.ReadDosage(base);
				base += nSample;
			} while (NodeVar.Next());
		}
		SET_DIMNAMES(rv_ans, R_Dosage_Name);
		UNPROTECT(1);
	}
	// output
	return rv_ans;
}

/// get dosage of alternative allele from 'genotype/data'
static SEXP get_dosage_alt(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	SEXP rv_ans = R_NilValue;
	ssize_t nSample  = File.SampleSelNum();
	ssize_t nVariant = File.VariantSelNum();
	if ((nSample > 0) && (nVariant > 0))
	{
		// initialize GDS genotype Node
		CApply_Variant_Dosage NodeVar(File, false, true);
		if (P->use_raw)
		{
			rv_ans = PROTECT(allocMatrix(RAWSXP, nSample, nVariant));
			C_UInt8 *base = (C_UInt8 *)RAW(rv_ans);
			do {
				NodeVar.ReadDosageAlt(base);
				base += nSample;
			} while (NodeVar.Next());
		} else {
			rv_ans = PROTECT(allocMatrix(INTSXP, nSample, nVariant));
			int *base = INTEGER(rv_ans);
			do {
				NodeVar.ReadDosageAlt(base);
				base += nSample;
			} while (NodeVar.Next());
		}
		SET_DIMNAMES(rv_ans, R_Dosage_Name);
		UNPROTECT(1);
	}
	// output
	return rv_ans;
}

/// get the number of alleles for each variant
static SEXP get_num_allele(CFileInfo &File, TVarMap &Var, void *param)
{
	TSelection &Sel = File.Selection();
	ssize_t num = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_INTEGER(num));
	CVectorRead<string> D(Var.Obj, Sel.pVariant, Sel.varStart, num);
	vector<string> buffer(1024);
	int n, *p = INTEGER(rv_ans);
	while ((n = D.Read(&buffer[0], 1024)) > 0)
	{
		for (int i=0; i < n; i++)
			*p++ = GetNumOfAllele(buffer[i].c_str());
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get the reference allele for each variant
static SEXP get_ref_allele(CFileInfo &File, TVarMap &Var, void *param)
{
	TSelection &Sel = File.Selection();
	ssize_t num = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_CHARACTER(num));
	CVectorRead<string> D(Var.Obj, Sel.pVariant, Sel.varStart, num);
	vector<string> buffer(1024);
	int n, k = 0;
	while ((n = D.Read(&buffer[0], 1024)) > 0)
	{
		for (int i=0; i < n; i++)
		{
			const char *p = buffer[i].c_str();
			size_t m = 0;
			for (const char *s=p; *s!=',' && *s!=0; s++) m++;
			SET_STRING_ELT(rv_ans, k++, mkCharLen(p, m));
		}
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get the alternative allele for each variant
static SEXP get_alt_allele(CFileInfo &File, TVarMap &Var, void *param)
{
	TSelection &Sel = File.Selection();
	ssize_t num = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_CHARACTER(num));
	CVectorRead<string> D(Var.Obj, Sel.pVariant, Sel.varStart, num);
	vector<string> buffer(1024);
	int n, k = 0;
	while ((n = D.Read(&buffer[0], 1024)) > 0)
	{
		for (int i=0; i < n; i++)
		{
			const char *p = buffer[i].c_str();
			for (; *p!=',' && *p!=0;) p++;
			if (*p == ',') p++;
			SET_STRING_ELT(rv_ans, k++, mkChar(p));
		}
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get the combination of chromosome and position ($chrom_pos)
static SEXP get_chrom_pos2(CFileInfo &File, TVarMap &Var, void *param)
{
	int n = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_CHARACTER(n));
	if (n > 0)
	{
		CChromIndex &Chrom = File.Chromosome();
		TSelection &Sel = File.Selection();
		const int *pos = &File.Position()[0];
		C_BOOL *s = Sel.pVariant + Sel.varStart;
		size_t p = 0, i = Sel.varStart;
		char buf1[1024] = { 0 };
		char buf2[1024] = { 0 };
		char *p1 = buf1, *p2 = buf2;
		int dup = 0;
		for (; n > 0; i++)
		{
			if (*s++)
			{
				const char *chr = Chrom[i].c_str();
				snprintf(p1, sizeof(buf1), "%s:%d", chr, pos[i]);
				if (strcmp(p1, p2) == 0)
				{
					dup ++;
					snprintf(p1, sizeof(buf1), "%s:%d_%d", chr, pos[i], dup);
					SET_STRING_ELT(rv_ans, p++, mkChar(p1));
				} else {
					char *tmp;
					tmp = p1; p1 = p2; p2 = tmp;
					SET_STRING_ELT(rv_ans, p++, mkChar(p2));
					dup = 0;
				}
				n--;
			}
		}
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get the combination of chromosome, position and allele ($chrom_pos_allele)
static SEXP get_chrom_pos_allele(CFileInfo &File, TVarMap &Var, void *param)
{
	TSelection &Sel = File.Selection();
	CChromIndex &Chrom = File.Chromosome();
	const int *PosBase = &File.Position()[0];
	ssize_t num = File.VariantSelNum();
	SEXP rv_ans = PROTECT(NEW_CHARACTER(num));
	CVectorRead<string> D(Var.Obj, Sel.pVariant, Sel.varStart, num);
	C_BOOL *psel = Sel.pVariant + Sel.varStart;
	vector<string> buffer(1024);
	char strbuf[8192] = { 0 };
	int n, k = 0;
	while ((n = D.Read(&buffer[0], 1024)) > 0)
	{
		for (int i=0; i < n; i++)
		{
			while (!*psel) psel++;
			size_t j = (psel++) - Sel.pVariant;
			const int pos = PosBase[j];
			const char *chr = Chrom[j].c_str();
			const char *allele = buffer[i].c_str();
			for (char *p=(char*)allele; *p; p++)
				if (*p == ',') *p = '_';
			snprintf(strbuf, sizeof(strbuf), "%s:%d_%s", chr, pos, allele);
			SET_STRING_ELT(rv_ans, k++, mkChar(strbuf));
		}
	}
	UNPROTECT(1);
	return rv_ans;
}

/// get the indices of selected samples ($sample_index)
static SEXP get_sample_index(CFileInfo &File, TVarMap &Var, void *param)
{
	ssize_t num = File.SampleSelNum();
	SEXP rv_ans = NEW_INTEGER(num);
	int *p = INTEGER(rv_ans), i = 0;
	const C_BOOL *s = File.Selection().pSample;
	while (num > 0)
		if (s[i++]) { *p++ = i; num--; }
	return rv_ans;
}

/// get the indices of selected variants ($variant_index)
static SEXP get_variant_index(CFileInfo &File, TVarMap &Var, void *param)
{
	TSelection &Sel = File.Selection();
	ssize_t num = File.VariantSelNum();
	SEXP rv_ans = NEW_INTEGER(num);
	int *p = INTEGER(rv_ans), i = Sel.varStart;
	const C_BOOL *s = Sel.pVariant;
	while (num > 0)
		if (s[i++]) { *p++ = i; num--; }
	return rv_ans;
}

/// get data from annotation/info/VARIABLE, TODO
static SEXP get_info(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	const C_UInt32 UseMode =
		GDS_R_READ_DEFAULT_MODE | (P->use_raw ? GDS_R_READ_ALLOW_RAW_TYPE : 0);

	SEXP rv_ans = R_NilValue;
	TSelection &Sel = File.Selection();
	CIndex &V = Var.Index;
	if (!V.HasIndex() || (P->padNA==TRUE && V.IsFixedOne()))
	{
		// no index or fixed-one
		Sel.GetStructVariant();
		C_BOOL *ss[2] = { Sel.pVariant+Sel.varStart, NULL };
		if (Var.NDim == 2)
			ss[1] = NeedArrayTRUEs(Var.Dim[1]);
		C_Int32 dimst[2]  = { C_Int32(Sel.varStart), 0 };
		C_Int32 dimcnt[2] = { C_Int32(Sel.varEnd-Sel.varStart), Var.Dim[1] };
		rv_ans = GDS_R_Array_Read(Var.Obj, dimst, dimcnt, ss, UseMode);
		if (Var.IsBit1)
		{
			PROTECT(rv_ans);
			rv_ans = AS_LOGICAL(rv_ans);
			UNPROTECT(1);
		}

	} else {
		// with index
		int var_start, var_count;
		vector<C_BOOL> var_sel;
		// get var_start, var_count, var_sel
		SEXP I32 = PROTECT(V.GetLen_Sel(Sel.pVariant, var_start, var_count,
			var_sel));

		C_BOOL *ss[2] = { &var_sel[0], NULL };
		C_Int32 dimst[2]  = { var_start, 0 };
		C_Int32 dimcnt[2] = { var_count, 0 };
		if (Var.NDim == 2)
		{
			GDS_Array_GetDim(Var.Obj, dimcnt, 2);
			dimcnt[0] = var_count;
		}
		SEXP val = PROTECT(GDS_R_Array_Read(Var.Obj, dimst, dimcnt, ss, UseMode));
		if (Var.IsBit1)
		{
			UNPROTECT(1);
			val = AS_LOGICAL(val);
			PROTECT(val);
		}

		if (P->padNA==TRUE && V.ValLenMax()==1 && Var.NDim==1)
		{
			// can be flatten
			int *psel = INTEGER(I32);
			size_t n = Rf_length(I32);
			rv_ans = PROTECT(Rf_allocVector(TYPEOF(val), n));
			switch (TYPEOF(val))
			{
			case INTSXP:
				{
					int *p = INTEGER(rv_ans), *s = INTEGER(val);
					for (size_t i=0; i < n; i++)
						p[i] = (psel[i]) ? (*s++) : NA_INTEGER;
					break;
				}
			case REALSXP:
				{
					double *p = REAL(rv_ans), *s = REAL(val);
					for (size_t i=0; i < n; i++)
						p[i] = (psel[i]) ? (*s++) : R_NaN;
					break;
				}
			case LGLSXP:
				{
					int *p = LOGICAL(rv_ans), *s = LOGICAL(val);
					for (size_t i=0; i < n; i++)
						p[i] = (psel[i]) ? (*s++) : NA_LOGICAL;
					break;
				}
			case STRSXP:
				{
					size_t s = 0;
					for (size_t i=0; i < n; i++)
					{
						SET_STRING_ELT(rv_ans, i,
							(psel[i]) ? STRING_ELT(val, s++) : NA_STRING);
					}
					break;
				}
			case RAWSXP:
				{
					Rbyte *p = RAW(rv_ans), *s = RAW(val);
					for (size_t i=0; i < n; i++)
						p[i] = (psel[i]) ? (*s++) : 0xFF;
					break;
				}
			default:
				throw ErrSeqArray("Not support data type for .padNA=TRUE.");
			}
		} else if (P->tolist)
		{
			// convert to a list
			const int n = Rf_length(I32);
			rv_ans = PROTECT(NEW_LIST(n));
			int *psel = INTEGER(I32);
			size_t d2 = (Var.NDim < 2) ? 1 : dimcnt[1], pt = 0;
			SEXP ZeroLen = NULL;
			for (int i=0; i < n; i++)
			{
				size_t nn = psel[i] * d2;
				SEXP vv;
				if (nn <= 0)
				{
					if (!ZeroLen) ZeroLen = Rf_allocVector(TYPEOF(val), 0);
					vv = ZeroLen;
				} else {
					vv = Rf_allocVector(TYPEOF(val), nn);
				}
				SET_ELEMENT(rv_ans, i, vv);
				if (nn > 0)
				{
					switch (TYPEOF(val))
					{
					case INTSXP:
						memcpy(INTEGER(vv), &INTEGER(val)[pt], sizeof(int)*nn);
						break;
					case REALSXP:
						memcpy(REAL(vv), &REAL(val)[pt], sizeof(double)*nn);
						break;
					case LGLSXP:
						memcpy(LOGICAL(vv), &LOGICAL(val)[pt], sizeof(int)*nn);
						break;
					case STRSXP:
						for (size_t i=0; i < nn; i++)
							SET_STRING_ELT(vv, i, STRING_ELT(val, pt+i));
						break;
					case RAWSXP:
						memcpy(RAW(vv), &RAW(val)[pt], nn);
						break;
					default:
						throw ErrSeqArray("Not support data type for .tolist=TRUE.");
					}
					pt += nn;
				}
			}
		} else {
			// create `list(length, data)`
			rv_ans = PROTECT(NEW_LIST(2));
				SET_ELEMENT(rv_ans, 0, I32);
				SET_ELEMENT(rv_ans, 1, val);
			SET_NAMES(rv_ans, R_Data_Name);
			SET_CLASS(rv_ans, R_Data_ListClass);
		}
		UNPROTECT(3);
	}
	// output
	return rv_ans;
}

/// get data from annotation/format/VARIABLE, TODO
static SEXP get_format(CFileInfo &File, TVarMap &Var, void *param)
{
	const TParam *P = (const TParam*)param;
	const C_UInt32 UseMode =
		GDS_R_READ_DEFAULT_MODE | GDS_R_READ_ALLOW_SP_MATRIX |
		(P->use_raw ? GDS_R_READ_ALLOW_RAW_TYPE : 0);

	SEXP rv_ans = R_NilValue;
	TSelection &Sel = File.Selection();
	Sel.GetStructVariant();
	CIndex &V = Var.Index;
	if (!V.HasIndex() || (P->padNA==TRUE && V.IsFixedOne()))
	{
		// no index or fixed-one
		Sel.GetStructVariant();
		C_BOOL *ss[2] = { Sel.pVariant+Sel.varStart, Sel.pSample };
		C_Int32 dimst[2]  = { C_Int32(Sel.varStart), 0 };
		C_Int32 dimcnt[2] = { C_Int32(Sel.varEnd-Sel.varStart), Var.Dim[1] };
		rv_ans = GDS_R_Array_Read(Var.Obj, dimst, dimcnt, ss, UseMode);
		if (XLENGTH(rv_ans) > 0)
			SET_DIMNAMES(rv_ans, R_Data_Dim2_Name);

	} else {
		int var_start, var_count;
		vector<C_BOOL> var_sel;
		SEXP I32 = PROTECT(V.GetLen_Sel(Sel.pVariant, var_start, var_count,
			var_sel));

		C_BOOL *ss[2] = { &var_sel[0], Sel.pSample };
		C_Int32 dimst[2]  = { var_start, 0 };
		C_Int32 dimcnt[2];
		GDS_Array_GetDim(Var.Obj, dimcnt, 2);
		dimcnt[0] = var_count;

		if (!P->tolist)
		{
			PROTECT(rv_ans = NEW_LIST(2));
				SET_ELEMENT(rv_ans, 0, I32);
				SEXP DAT = GDS_R_Array_Read(Var.Obj, dimst, dimcnt, ss, UseMode);
				SET_ELEMENT(rv_ans, 1, DAT);
				SET_NAMES(rv_ans, R_Data_Name);
				if (XLENGTH(DAT) > 0)
					SET_DIMNAMES(DAT, R_Data_Dim2_Name);
				SET_CLASS(rv_ans, R_Data_ListClass);
			UNPROTECT(2);
		} else {
			// check
			SEXP val = PROTECT(GDS_R_Array_Read(Var.Obj, dimst, dimcnt, ss,
				UseMode));
			switch (TYPEOF(val))
			{
				case INTSXP: case REALSXP: case LGLSXP:
				case STRSXP: case RAWSXP:
					break;
				default:
					throw ErrSeqArray("Not support data type for .tolist=TRUE.");
			}
			// convert to a list
			const int n = Rf_length(I32);
			rv_ans = PROTECT(NEW_LIST(n));
			int *psel = INTEGER(I32);
			size_t d2 = File.SampleNum(), pt = 0;
			SEXP ZeroLen = NULL;
			for (int i=0; i < n; i++)
			{
				SEXP vv;
				size_t nn = psel[i] * d2;
				if (nn <= 0)
				{
					if (!ZeroLen) ZeroLen = Rf_allocMatrix(TYPEOF(val), d2, 0);
					vv = ZeroLen;
				} else {
					vv = Rf_allocMatrix(TYPEOF(val), d2, psel[i]);
				}
				SET_ELEMENT(rv_ans, i, vv);
				if (nn > 0)
				{
					switch (TYPEOF(val))
					{
					case INTSXP:
						memcpy(INTEGER(vv), &INTEGER(val)[pt], sizeof(int)*nn);
						break;
					case REALSXP:
						memcpy(REAL(vv), &REAL(val)[pt], sizeof(double)*nn);
						break;
					case LGLSXP:
						memcpy(LOGICAL(vv), &LOGICAL(val)[pt], sizeof(int)*nn);
						break;
					case STRSXP:
						for (size_t i=0; i < nn; i++)
							SET_STRING_ELT(vv, i, STRING_ELT(val, pt+i));
						break;
					case RAWSXP:
						memcpy(RAW(vv), &RAW(val)[pt], nn);
						break;
					}
					pt += nn;
				}
			}
			UNPROTECT(3);
		}
	}
	// output
	return rv_ans;
}

/// get data from R objects
static SEXP get_env_R(CFileInfo &File, TVarMap &Var, void *param)
{
	const char *varnm = Var.Name.c_str();
	SEXP Env = ((TParam*)param)->Env, rv_ans, x;
	rv_ans = x = R_NilValue;
	if (!Rf_isNull(Env))
	{
		if (Rf_isEnvironment(Env))
		{
			SEXP v = Rf_findVarInFrame(Env, Rf_install(varnm));
			if (v != R_UnboundValue) x = v;
		} else if (Rf_isVectorList(Env))
			x = RGetListElement(Env, varnm);
	}
	if (Rf_isNull(x))
		throw ErrSeqArray("No variable '%s' in the enviroment or list.", varnm);
	if (!Rf_isVector(x))
		throw ErrSeqArray("'%s' should be a vector.", varnm);
	if (Rf_length(x) != File.VariantNum())
		throw ErrSeqArray("'length(%s)' should be the same as the number of variants.", varnm);

	TSelection &Sel = File.Selection();
	int n = File.VariantSelNum();
	if (n != File.VariantNum())
	{
		int nprot = 1;
		PROTECT(x);
		if (Rf_isInteger(x))
		{
			rv_ans = PROTECT(NEW_INTEGER(n)); nprot++;
			int *b = INTEGER(x), *p = INTEGER(rv_ans);
			size_t i = Sel.varStart;
			C_BOOL *s = Sel.pVariant;
			for (; n > 0; i++)
				if (s[i]) { *p++ = b[i]; n--; }
		} else if (Rf_isLogical(x))
		{
			rv_ans = PROTECT(NEW_LOGICAL(n)); nprot++;
			int *b = INTEGER(x), *p = INTEGER(rv_ans);
			size_t i = Sel.varStart;
			C_BOOL *s = Sel.pVariant;
			for (; n > 0; i++)
				if (s[i]) { *p++ = b[i]; n--; }
		} else if (Rf_isReal(x))
		{
			rv_ans = PROTECT(NEW_NUMERIC(n)); nprot++;
			double *b = REAL(x), *p = REAL(rv_ans);
			size_t i = Sel.varStart;
			C_BOOL *s = Sel.pVariant;
			for (; n > 0; i++)
				if (s[i]) { *p++ = b[i]; n--; }
		} else if (Rf_isString(x))
		{
			rv_ans = PROTECT(NEW_CHARACTER(n)); nprot++;
			size_t p=0, i=Sel.varStart;
			C_BOOL *s = Sel.pVariant;
			for (; n > 0; i++)
			{
				if (s[i])
				{
					SET_STRING_ELT(rv_ans, p++, STRING_ELT(x, i));
					n--;
				}
			}
		} else {
			throw ErrSeqArray("No implementation, ask the package maintainer.");
		}
		UNPROTECT(nprot);
	} else {
		rv_ans = x;
	}
	return rv_ans;
}


/// get TVarMap from a variable name
COREARRAY_DLL_LOCAL TVarMap &VarGetStruct(CFileInfo &File, const string &name)
{
	// find variable
	map<string, TVarMap> &VarMap = File.VarMap();
	map<string, TVarMap>::iterator it = VarMap.find(name);
	if (it == VarMap.end())
	{
		// if it is not initialized
		TVarMap vm;
		if (name == VAR_SAMP_ID)
		{
			vm.Init(File, name, get_sample_1d);
			CHECK_SAMPLE_DIMENSION
		} else if (name == VAR_POSITION)
		{
			vm.Init(File, name, get_position);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_CHROM)
		{
			vm.Init(File, name, get_chrom);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name==VAR_ID || name==VAR_ALLELE || name==VAR_ANNOT_ID ||
			name==VAR_ANNOT_QUAL || name==VAR_ANNOT_FILTER)
		{
			vm.Init(File, name, get_data_1d);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_GENOTYPE)
		{
			vm.Init(File, "genotype/data", get_genotype);
		} else if (name == VAR_GENO_INDEX)
		{
			vm.Init(File, "genotype/@data", get_data_1d);  // TODO
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_PHASE)
		{
			vm.Init(File, "phase/data", get_phase);
			if ((vm.NDim < 2) || (vm.NDim > 3) ||
					(vm.Dim[0] != File.VariantNum()) ||
					(vm.Dim[1] != File.SampleNum()))
				throw ErrSeqArray(ERR_DIM, vm.Name.c_str());
		} else
		// ===========================================================
		// internally generated variables
		if (name == VAR_DOSAGE)
		{
			vm.Func = get_dosage;
		} else if (name == VAR_DOSAGE_ALT)
		{
			vm.Func = get_dosage_alt;
		} else if (name == VAR_NUM_ALLELE)
		{
			vm.Init(File, VAR_ALLELE, get_num_allele);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_REF_ALLELE)
		{
			vm.Init(File, VAR_ALLELE, get_ref_allele);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_ALT_ALLELE)
		{
			vm.Init(File, VAR_ALLELE, get_alt_allele);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_CHROM_POS2)
		{
			vm.Init(File, VAR_CHROM, get_chrom_pos2);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_CHROM_POS_ALLELE)
		{
			vm.Init(File, VAR_ALLELE, get_chrom_pos_allele);
			CHECK_VARIANT_ONE_DIMENSION
		} else if (name == VAR_SAMPLE_INDEX)
		{
			vm.Func = get_sample_index;
		} else if (name == VAR_VARIANT_INDEX)
		{
			vm.Func = get_variant_index;
		} else 
		// ===========================================================
		// annotation info
		if (strncmp(name.c_str(), "annotation/info/", 16) == 0)
		{
			if (name.find('@') == string::npos)
			{
				vm.InitWtIndex(File, name, get_info);
				if ((vm.NDim != 1) && (vm.NDim != 2))
					throw ErrSeqArray(ERR_DIM, name.c_str());
			} else {
				vm.Init(File, name, get_data_1d);  // TODO
				CHECK_VARIANT_ONE_DIMENSION
			}
		} else
		// ===========================================================
		// annotation format
		if (strncmp(name.c_str(), "annotation/format/@", 19) == 0)
		{
			string name2(name);
			name2.erase(18, 1).append("/@data");
			vm.Init(File, name2, get_data_1d);  // TODO
			CHECK_VARIANT_ONE_DIMENSION
		} else if (strncmp(name.c_str(), "annotation/format/", 18) == 0)
		{
			vm.InitWtIndex(File, name + "/data", get_format);
			if (vm.NDim != 2)
				throw ErrSeqArray(ERR_DIM, vm.Name.c_str());
		} else
		// ===========================================================
		// annotation sample
		if (strncmp(name.c_str(), "sample.annotation/", 18) == 0)
		{
			vm.Init(File, name, get_sample_1d);
			CHECK_SAMPLE_DIMENSION
		} else
		// ===========================================================
		// R variables in the environment or list
		if (name[0]=='$' && name[1]==':')
		{
			vm.Name.assign(name.c_str() + 2);
			vm.Func = get_env_R;
		} else
		// ===========================================================
		// invalid variable name
		{
			throw ErrSeqArray(
				"'%s' is not a standard variable name, and the standard format:\n"
				"    sample.id, variant.id, position, chromosome, allele, genotype\n"
				"    annotation/id, annotation/qual, annotation/filter\n"
				"    annotation/info/VARIABLE_NAME, annotation/format/VARIABLE_NAME\n"
				"    sample.annotation/VARIABLE_NAME, etc", name.c_str());
		}

		VarMap[name] = vm;
		it = VarMap.find(name);
	}
	return it->second;
}


/// get data from a SeqArray GDS file
static SEXP VarGetData(CFileInfo &File, const string &name, int use_raw,
	int padNA, int tolist, SEXP Env)
{
	TVarMap &vm = VarGetStruct(File, name);
	if (vm.Obj)
	{
		PdGDSObj node;
		int node_id;
		if (GDS_Node_Load(vm.Obj, vm.ObjID, name.c_str(), File.File(),
			&node, &node_id))
		{
			vm.Obj = node;
			vm.ObjID = node_id;
		}
	}
	TParam param(use_raw, padNA, tolist, Env);
	return (*vm.Func)(File, vm, &param);
}

}


// =======================================================================

using namespace SeqArray;

extern "C"
{

/// Get data from a working space
COREARRAY_DLL_EXPORT SEXP SEQ_GetData(SEXP gdsfile, SEXP var_name, SEXP UseRaw,
	SEXP PadNA, SEXP ToList, SEXP Env)
{
	// var.name
	if (!Rf_isString(var_name))
		error("'var.name' should be character.");
	const int nlen = RLength(var_name);
	if (nlen <= 0)
		error("'length(var.name)' should be > 0.");
	// .useraw
	if (TYPEOF(UseRaw) != LGLSXP)
		error("'.useraw' must be logical.");
	const int use_raw = Rf_asLogical(UseRaw);
	// .padNA
	const int padNA = Rf_asLogical(PadNA);
	if (padNA == NA_LOGICAL)
		error("'.padNA' must be TRUE or FALSE.");
	// .tolist
	const int tolist = Rf_asLogical(ToList);
	if (tolist == NA_LOGICAL)
		error("'.tolist' must be TRUE or FALSE.");
	// .envir
	if (!Rf_isNull(Env))
	{
		if (!Rf_isEnvironment(Env) && !Rf_isVectorList(Env))
			error("'envir' should be an environment and list object.");
	}

	COREARRAY_TRY
		// File information
		CFileInfo &File = GetFileInfo(gdsfile);
		// Get data
		if (nlen == 1)
		{
			rv_ans = VarGetData(File, CHAR(STRING_ELT(var_name, 0)), use_raw,
				padNA, tolist, Env);
		} else {
			rv_ans = PROTECT(NEW_LIST(nlen));
			for (int i=0; i < nlen; i++)
			{
				SET_VECTOR_ELT(rv_ans, i,
					VarGetData(File, CHAR(STRING_ELT(var_name, i)), use_raw,
					padNA, tolist, Env));
			}
			setAttrib(rv_ans, R_NamesSymbol, getAttrib(var_name, R_NamesSymbol));
			UNPROTECT(1);
		}
	COREARRAY_CATCH
}


COREARRAY_DLL_LOCAL extern const char *Txt_Apply_VarIdx[];


/// Apply functions over variants in block
COREARRAY_DLL_EXPORT SEXP SEQ_BApply_Variant(SEXP gdsfile, SEXP var_name,
	SEXP FUN, SEXP as_is, SEXP var_index, SEXP param, SEXP rho)
{
	// bsize
	int bsize = Rf_asInteger(RGetListElement(param, "bsize"));
	if (bsize < 1)
		error("'bsize' must be >= 1.");
	// .useraw
	SEXP pam_use_raw = RGetListElement(param, "useraw");
	if (!Rf_isLogical(pam_use_raw))
		error("'.useraw' must be TRUE, FALSE or NA.");
	int use_raw_flag = Rf_asLogical(pam_use_raw);
	// .padNA
	int padNA = Rf_asLogical(RGetListElement(param, "padNA"));
	if (padNA == NA_LOGICAL)
		error("'.padNA' must be TRUE or FALSE.");
	// .tolist
	int tolist = Rf_asLogical(RGetListElement(param, "tolist"));
	if (tolist == NA_LOGICAL)
		error("'.tolist' must be TRUE or FALSE.");
	// .progress
	int prog_flag = Rf_asLogical(RGetListElement(param, "progress"));
	if (prog_flag == NA_LOGICAL)
		error("'.progress' must be TRUE or FALSE.");

	COREARRAY_TRY

		// File information
		CFileInfo &File = GetFileInfo(gdsfile);
		File.VarMap().clear();

		// Selection
		TSelection &Selection = File.Selection();

		// the number of selected variants
		int nVariant = File.VariantSelNum();
		if (nVariant <= 0)
			throw ErrSeqArray("There is no selected variant.");

		// the number of data blocks
		int NumBlock = nVariant / bsize;
		if (nVariant % bsize) NumBlock ++;

		// the number of calling PROTECT
		int nProtected = 0;

		// as.is
		Rconnection OutputConn = NULL;
		PdGDSObj OutputGDS = NULL;
		int DatType;
		if (Rf_inherits(as_is, "connection"))
		{
			OutputConn = R_GetConnection(as_is);
			DatType = 2;
		} else if (Rf_inherits(as_is, "gdsn.class"))
		{
			OutputGDS = GDS_R_SEXP2Obj(as_is, FALSE);
			DatType = 3;
		} else {
			const char *s = CHAR(STRING_ELT(as_is, 0));
			if (strcmp(s, "list")==0 || strcmp(s, "unlist")==0)
			{
				DatType = 1;
				rv_ans = PROTECT(NEW_LIST(NumBlock)); nProtected ++;
			} else {
				DatType = 0;
			}
		}

		// rho environment
		if (!isEnvironment(rho))
			throw ErrSeqArray("'rho' should be an environment");

		// var.index
		int VarIdx = MatchText(CHAR(STRING_ELT(var_index, 0)), Txt_Apply_VarIdx);
		if (VarIdx < 0)
			throw ErrSeqArray("'var.index' is not valid!");
		SEXP R_Index = R_NilValue;
		if (VarIdx > 0)
		{
			PROTECT(R_Index = NEW_INTEGER(1));
			nProtected ++;
		}

		// calling
		SEXP R_fcall = R_NilValue;
		SEXP R_call_param = R_NilValue;
		int num_var = RLength(var_name);
		if (num_var > 1)
		{
			PROTECT(R_call_param = NEW_LIST(num_var));
			nProtected ++;
			// set name to R_call_param
			SET_NAMES(R_call_param, GET_NAMES(var_name));
			// make a call function
			if (VarIdx > 0)
			{
				PROTECT(R_fcall = LCONS(FUN, LCONS(R_Index,
					LCONS(R_call_param, LCONS(R_DotsSymbol, R_NilValue)))));
				nProtected ++;
			} else {
				PROTECT(R_fcall = LCONS(FUN,
					LCONS(R_call_param, LCONS(R_DotsSymbol, R_NilValue))));
				nProtected ++;
			}
		}


		// local selection
		TSelection &Sel = File.Push_Selection(true, false);
		Sel.ClearStructVariant();
		memset(Sel.pVariant, 0, File.VariantNum());

		C_BOOL *pBase, *pSel, *pEnd;
		pBase = pSel = Selection.pVariant;
		pEnd = pBase + File.VariantNum();

		// progress object
		CProgressStdOut progress(NumBlock, 1, prog_flag);

		// for-loop
		for (int idx=0; idx < NumBlock; idx++)
		{
			switch (VarIdx)
			{
			case 1:  // relative
				INTEGER(R_Index)[0] = idx*bsize + 1; break;
			case 2:  // absolute
				while ((pSel < pEnd) && (*pSel == FALSE))
					pSel ++;
				INTEGER(R_Index)[0] = pSel - pBase + 1; break;
			}

			// assign sub-selection
			{
				// clear selection
				Sel.ClearSelectVariant();
				// find the first TRUE
				pSel = VEC_BOOL_FIND_TRUE(pSel, pEnd);
				Sel.varStart = pSel - pBase;
				// for-loop
				C_BOOL *pNewSel = Sel.pVariant;
				int bs = bsize;
				for (; bs > 0; bs--)
				{
					while ((pSel < pEnd) && (*pSel == FALSE))
						pSel ++;
					if (pSel < pEnd)
					{
						pNewSel[pSel - pBase] = TRUE;
						pSel ++;
					} else
						break;
				}
				Sel.varTrueNum = bsize - bs;
				Sel.varEnd = pSel - pBase;
			}

			// load data and call the user-defined function
			SEXP call_val = R_NilValue;
			if (num_var > 1)
			{
				for (int i=0; i < num_var; i++)
				{
					SET_ELEMENT(R_call_param, i,
						VarGetData(File, CHAR(STRING_ELT(var_name, i)),
						use_raw_flag, padNA, tolist, rho));
				}
				// call R function
				call_val = eval(R_fcall, rho);

			} else {
				R_call_param = VarGetData(File, CHAR(STRING_ELT(var_name, 0)),
					use_raw_flag, padNA, tolist, rho);
				// make a call function
				if (VarIdx > 0)
				{
					PROTECT(R_fcall = LCONS(FUN, LCONS(R_Index,
						LCONS(R_call_param, LCONS(R_DotsSymbol, R_NilValue)))));
				} else {
					PROTECT(R_fcall = LCONS(FUN,
						LCONS(R_call_param, LCONS(R_DotsSymbol, R_NilValue))));
				}

				// call R function
				call_val = eval(R_fcall, rho);
			}

			// store data
			switch (DatType)
			{
			case 1:  // list
				SET_ELEMENT(rv_ans, idx, call_val);
				break;
			case 2:  // connection
				if (OutputConn->text)
				{
					if (Rf_isList(call_val))
					{
						throw ErrSeqArray("the user-defined function should return a character vector.");
					} else if (!Rf_isString(call_val))
					{
						call_val = AS_CHARACTER(call_val);
					}
					size_t n = XLENGTH(call_val);
					for (size_t i=0; i < n; i++)
					{
						ConnPutText(OutputConn, "%s\n", CHAR(STRING_ELT(call_val, i)));
					}
				} else {
					if (TYPEOF(call_val) != RAWSXP)
						throw ErrSeqArray("the user-defined function should return a RAW vector.");
					size_t n = XLENGTH(call_val);
					size_t m = R_WriteConnection(OutputConn, RAW(call_val), n);
					if (n != m)
						throw ErrSeqArray("error in writing to a connection.");
				}
				break;
			case 3:  // gdsn.class
				RAppendGDS(OutputGDS, call_val);
				break;
			}

			// release R_fcall
			if (num_var <= 1) UNPROTECT(1);

			progress.Forward();
		}

		File.Pop_Selection();

		// finally
		UNPROTECT(nProtected);

	COREARRAY_CATCH
}

} // extern "C"

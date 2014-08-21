/*
 * execfunc.cpp
 *
 *  Created on: Aug 6, 2014
 *      Author: imdb
 */
#include"execfunc.h"
#include "qnode.h"
#include "../../common/TypeCast.h"
bool ExecEvalQual(vector<QNode *>v_qual,void *tuple,Schema *schema)
{
	assert(tuple!=NULL);
	for(int i=0;i<v_qual.size();i++)
	{
		QNode *qual=v_qual[i];
		void * result=((*(qual->FuncId))(qual,tuple,schema));
		if(*(bool *)result!=true)
			return false;
	}
	return true;

}


void *Exec_unary(Node *cinfo,void *tuple,Schema *schema)
{
	QExpr_unary *unode=(QExpr_unary *)cinfo;
	FuncCallInfoData finfo;
	finfo.args[0]=unode->next->FuncId(unode->next,tuple,schema);
	finfo.nargs=1;
	finfo.results=unode->result_store;
	unode->function_call(&finfo);
	return TypeCast::type_cast_func[unode->actual_type][unode->return_type](finfo.results,unode->result_store);
}
void *Exec_cal(Node *cinfo,void *tuple,Schema *schema)
{
	QExpr_binary *cal=(QExpr_binary *)(cinfo);
	FuncCallInfoData finfo;
	finfo.args[0]=cal->lnext->FuncId(cal->lnext,tuple,schema);
	finfo.args[1]=cal->rnext->FuncId(cal->rnext,tuple,schema);
	finfo.nargs=2;
	finfo.results=cal->result_store;
	cal->function_call(&finfo);
	return TypeCast::type_cast_func[cal->actual_type][cal->return_type](finfo.results,cal->result_store);
}
void *Exec_cmp(Node *cinfo,void *tuple,Schema *schema)
{
	QExpr_binary *cal=(QExpr_binary *)(cinfo);
	FuncCallInfoData finfo;
	finfo.args[0]=cal->lnext->FuncId(cal->lnext,tuple,schema);
	finfo.args[1]=cal->rnext->FuncId(cal->rnext,tuple,schema);
	finfo.nargs=2;
	finfo.results=cal->result_store;
	cal->function_call(&finfo);
	return finfo.results;//the actual type is bool ,so it needn't change
}
void *Exec_ternary(Node *cinfo,void *tuple,Schema *schema)
{
	QExpr_ternary *tnode=(QExpr_ternary *)cinfo;
	FuncCallInfoData finfo;
	finfo.args[0]=tnode->next0->FuncId(tnode->next0,tuple,schema);
	finfo.args[1]=tnode->next1->FuncId(tnode->next1,tuple,schema);
	finfo.args[2]=tnode->next2->FuncId(tnode->next2,tuple,schema);
	finfo.nargs=3;
	finfo.results=tnode->result_store;
	tnode->function_call(&finfo);
	return TypeCast::type_cast_func[tnode->actual_type][tnode->return_type](finfo.results,tnode->result_store);
}

void *getConst(Node *cinfo,void *tuple,Schema *schema)//TODO string=>actual_type=>return_type
{
	QExpr *qexpr=(QExpr*)(cinfo);
	return qexpr->result_store;
}
void *getcol(Node *cinfo,void *tuple,Schema *schema)//TODO need actual_type=>return_type
{
	QColcumns *qcol=(QColcumns *)(cinfo);
	void *result=schema->getColumnAddess(qcol->id,tuple);
	return TypeCast::type_cast_func[qcol->actual_type][qcol->return_type](result,qcol->result_store);
}
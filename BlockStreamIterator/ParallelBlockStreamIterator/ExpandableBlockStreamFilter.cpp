/*
 * ExpandableBlockStreamFilter.cpp
 *
 *  Created on: Aug 28, 2013
 *      Author: wangli
 */
#include <limits>
#include "ExpandableBlockStreamFilter.h"
#include "../../utility/warmup.h"
#include "../../utility/rdtsc.h"
#include <assert.h>
#include "../../common/ExpressionCalculator.h"
#include "../../common/Expression/execfunc.h"
#include "../../common/Expression/qnode.h"
#include "../../Parsetree/sql_node_struct.h"
#include "../../common/Expression/initquery.h"
#include "../../common/Expression/queryfunc.h"
#include "../../common/data_type.h"
#include "../../Config.h"
#include "../../codegen/ExpressionGenerator.h"

#define NEWCONDITION

ExpandableBlockStreamFilter::ExpandableBlockStreamFilter(State state) :
		state_(state),generated_filter_function_(0),generated_filter_processing_fucntoin_(0) {
	initialize_expanded_status();
	initialize_operator_function();
}

ExpandableBlockStreamFilter::ExpandableBlockStreamFilter():generated_filter_function_(0),generated_filter_processing_fucntoin_(0) {
	initialize_expanded_status();
}

ExpandableBlockStreamFilter::~ExpandableBlockStreamFilter() {
}
ExpandableBlockStreamFilter::State::State(Schema* schema,
		BlockStreamIteratorBase* child, vector<QNode *> qual,
		map<string, int> colindex, unsigned block_size) :
		schema_(schema), child_(child), qual_(qual), colindex_(colindex), block_size_(
				block_size) {

}
ExpandableBlockStreamFilter::State::State(Schema* schema,
		BlockStreamIteratorBase* child,
		std::vector<AttributeComparator> comparator_list, unsigned block_size) :
		schema_(schema), child_(child), comparator_list_(comparator_list), block_size_(
				block_size) {

}
bool ExpandableBlockStreamFilter::open(const PartitionOffset& part_off) {

	AtomicPushFreeBlockStream(
			BlockStreamBase::createBlock(state_.schema_, state_.block_size_));

	filter_thread_context* ftc = new filter_thread_context();
	ftc->block_for_asking_ = BlockStreamBase::createBlock(state_.schema_,
			state_.block_size_);
	ftc->temp_block_ = BlockStreamBase::createBlock(state_.schema_,
			state_.block_size_);
	ftc->block_stream_iterator_ = ftc->block_for_asking_->createIterator();
	ftc->thread_qual_=state_.qual_;
	for(int i=0;i<state_.qual_.size();i++)
	{
		Expr_copy(state_.qual_[i],ftc->thread_qual_[i]);
		InitExprAtPhysicalPlan(ftc->thread_qual_[i]);
	}
	initContext(ftc);

	if (tryEntryIntoSerializedSection()) {
		tuple_after_filter_ = 0;
		if(Config::enable_codegen){
			ticks start=curtick();
			generated_filter_processing_fucntoin_=getFilterProcessFunc(state_.qual_[0],state_.schema_);
			if(generated_filter_processing_fucntoin_)
				printf("CodeGen (full feature) succeeds!(%f8.4ms)\n",getMilliSecond(start));
			else{
				generated_filter_function_=getExprFunc(state_.qual_[0],state_.schema_);
				if(generated_filter_function_){
					ff_=computeFilterwithGeneratedCode;
					printf("CodeGen (partial feature) succeeds!(%f8.4ms)\n",getMilliSecond(start));
				}
				else{
					ff_=computeFilter;
					printf("CodeGen fails!\n");
				}
			}
		}
		else{
			ff_=computeFilter;
			printf("CodeGen fails!\n");
		}
//		if(generated_filter_function_){
//			ff_=computeFilterwithGeneratedCode;
//		}
//		else{
//			printf("CodeGen fails!\n");
//		}
		const bool child_open_return = state_.child_->open(part_off);
		setOpenReturnValue(child_open_return);
		broadcaseOpenFinishedSignal();
	} else {
		waitForOpenFinished();
		return state_.child_->open(part_off);
	}
}

bool ExpandableBlockStreamFilter::next(BlockStreamBase* block) {

	void* tuple_from_child;
	void* tuple_in_block;
	filter_thread_context* tc = (filter_thread_context*) getContext();
	while(true){
		if(tc->block_stream_iterator_->currentTuple()==0){
			/* mark the block as processed by setting it empty*/
			tc->block_for_asking_->setEmpty();
			if(state_.child_->next(tc->block_for_asking_)){
//				printf("%lld\n",pthread_self());
				delete tc->block_stream_iterator_;
				tc->block_stream_iterator_=tc->block_for_asking_->createIterator();
			}
			else
			{
				if (!block->Empty()) {
					return true;
				} else {
					return false;
				}
			}
		}
		process_logic(block,tc);
		/* there are totally two reasons for the end of the while loop.
		 * (1) block is full of tuples satisfying filter (should return true to the caller)
		 * (2) block_for_asking_ is exhausted (should fetch a new block from child and continue to process)
		 */
		if(block->Full())
			// for case (1)
			return true;
		else{
		}
	}
}
void ExpandableBlockStreamFilter::process_logic(BlockStreamBase* block,filter_thread_context* tc) {
	if(generated_filter_processing_fucntoin_){
		int b_cur=block->getTuplesInBlock();
		int c_cur=tc->block_stream_iterator_->get_cur();
		const int b_tuple_count=block->getBlockCapacityInTuples();
		const int c_tuple_count=tc->block_for_asking_->getTuplesInBlock();

		generated_filter_processing_fucntoin_(block->getBlock(),&b_cur,b_tuple_count,tc->block_for_asking_->getBlock(),&c_cur,c_tuple_count);
//		process_func(block->getBlock(),&b_cur,b_tuple_count,tc->block_for_asking_->getBlock(),&c_cur,c_tuple_count,state_.schema_->getTupleMaxSize(),generated_filter_function_);
		((BlockStreamFix*)block)->setTuplesInBlock(b_cur);
		tc->block_stream_iterator_->set_cur(c_cur);
	}
	else{
		void* tuple_from_child;
		void* tuple_in_block;
		while ((tuple_from_child = tc->block_stream_iterator_->currentTuple()) > 0) {
			bool pass_filter = true;
	#ifdef NEWCONDITION
			ff_(pass_filter,tuple_from_child,generated_filter_function_,state_.schema_,tc->thread_qual_);
	#else
			pass_filter=true;
			for(unsigned i=0;i<state_.comparator_list_.size();i++){

				if(!state_.comparator_list_[i].filter(state_.schema_->getColumnAddess(state_.comparator_list_[i].get_index(),tuple_from_child))){
					pass_filter=false;
					break;
				}
			}
	#endif
			if (pass_filter) {
				const unsigned bytes = state_.schema_->getTupleActualSize(
						tuple_from_child);
				if ((tuple_in_block = block->allocateTuple(bytes)) > 0) {
					block->insert(tuple_in_block, tuple_from_child, bytes);
					tuple_after_filter_++;
				} else {
					/* we have got a block full of result tuples*/
					return;
				}
			}
			/* point the iterator to the next tuple */
			tc->block_stream_iterator_->increase_cur_();
		}
		/* mark the block as processed by setting it empty*/
		tc->block_for_asking_->setEmpty();
	}
}

bool ExpandableBlockStreamFilter::close() {
	initialize_expanded_status();
	open_finished_ = false;

	for (unsigned i = 0; i < free_block_stream_list_.size(); i++) {
		delete free_block_stream_list_.front();
		free_block_stream_list_.pop_front();
	}

	destoryAllContext();

	free_block_stream_list_.clear();
	state_.child_->close();
	return true;
}

void ExpandableBlockStreamFilter::print() {
//	printf("Filter size=%d\n",state_.v_ei_.size());

	printf("filter: \n");
	for(int i=0;i<state_.qual_.size();i++)
	{
		printf("	%s\n",state_.qual_[i]->alias.c_str());
	}
	state_.child_->print();
}
bool ExpandableBlockStreamFilter::atomicPopRemainingBlock(
		remaining_block & rb) {
	lock_.acquire();

	if (remaining_block_list_.size() > 0) {
		rb = remaining_block_list_.front();
		remaining_block_list_.pop_front();

		lock_.release();

		return true;
	} else {

		lock_.release();

		return false;
	}
}

void ExpandableBlockStreamFilter::atomicPushRemainingBlock(remaining_block rb) {
	lock_.acquire();
	remaining_block_list_.push_back(rb);
	lock_.release();
}

BlockStreamBase* ExpandableBlockStreamFilter::AtomicPopFreeBlockStream() {
	assert(!free_block_stream_list_.empty());
	lock_.acquire();
	BlockStreamBase *block = free_block_stream_list_.front();
	free_block_stream_list_.pop_front();
	lock_.release();
	return block;
}
void ExpandableBlockStreamFilter::AtomicPushFreeBlockStream(
		BlockStreamBase* block) {
	lock_.acquire();
	free_block_stream_list_.push_back(block);
	lock_.release();
}
thread_context ExpandableBlockStreamFilter::popContext() {
	lock_.acquire();
	assert(context_list_.find(pthread_self()) != context_list_.cend());
	thread_context ret = context_list_[pthread_self()];
	context_list_.erase(pthread_self());
//	printf("Thread %lx is poped!\n",pthread_self());
	lock_.release();
	return ret;
}

void ExpandableBlockStreamFilter::pushContext(const thread_context& tc) {
	lock_.acquire();
	assert(context_list_.find(pthread_self()) == context_list_.cend());
	context_list_[pthread_self()] = tc;
//	printf("Thread %lx is pushed!\n",pthread_self());
	lock_.release();
}
//void ExpandableBlockStreamFilter::destoryContext(thread_context& tc){
//	lock_.acquire();
//	assert(context_list_.find(pthread_self())!=context_list_.cend());
//	context_list_[pthread_self()].block_for_asking_->~BlockStreamBase();
//	context_list_[pthread_self()].iterator_->~BlockStreamTraverseIterator();
//	context_list_.erase(pthread_self());
//	lock_.release();
////	tc->block_for_asking_->~BlockStreamBase();
////	tc->iterator_->~BlockStreamTraverseIterator();
//}

void ExpandableBlockStreamFilter::computeFilter(bool& ret, void* tuple,expr_func func_gen, Schema* schema, vector<QNode*> thread_qual_) {
	ret=ExecEvalQual(thread_qual_, tuple,	schema);
}


void ExpandableBlockStreamFilter::computeFilterwithGeneratedCode(bool& ret, void* tuple, expr_func func_gen, Schema* schema, vector<QNode*> allocator) {
	func_gen(tuple,&ret);
}

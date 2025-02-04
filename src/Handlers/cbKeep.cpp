#include "cbKeep.h"
std::string cbKeep::xmlname = "Keep";
#include "../HandlerFactory.h"

int cbKeep::Init () {
		Callback::Init();
		pugi::xml_attribute attr = node.attribute("What");
		if (attr) {
			std::string nm = attr.value();
			const Model::Global& it = solver->lattice->model->globals.by_name(nm);
			if (!it) {
        			error("Unknown Global %s in %s\n", nm.c_str(), node.name());
        			return -1;
                        }
                        what = it.id;
			whatInObj = it.inObjId;
		} else {
			error("No What attribute in %s\n", node.name());
			return -1;
		}
		if ( (attr = node.attribute("Above")) ) {
			thr = attr.as_double();
                        my_type = 1;
		} else if ( (attr = node.attribute("Below")) ) {
			thr = attr.as_double();
                        my_type = -1;
		} else if ( (attr = node.attribute("Equal")) ) {
			thr = attr.as_double();
                        my_type = 0;
		} else {
			error("%s should have Above, Below or Equal attribute\n", node.name());
			return -1;		        
		}
		if ( (attr = node.attribute("Force")) ) {
			force = attr.as_double();
		} else {
		        force = 1;
                }
		old_iter_type = solver->iter_type;
		solver->iter_type |= ITER_LASTGLOB;
		return 0;
	}


int cbKeep::DoIt () {
		Callback::DoIt();
                double s = 0.0;
                if (solver->mpi_rank == 0) {
                        double v = solver->lattice->globals[ what ];
                        output("Keep: %le compared to %le\n", v, thr);
                        v = (thr - v)*force;
                        switch (my_type) {
                        case -1:
                                if (v<0) s=v;
                                break;
                        case 0:
                                s=v;
                                break;
                        case 1:
                                if (v>0) s=v;
                                break;
                        }
                }
                MPI_Bcast(&s, 1, MPI_INT, 0, MPMD.local);
		solver->lattice->zSet.set(whatInObj, -1, s);
		return 0;
	}


int cbKeep::Finish () {
		solver->iter_type = old_iter_type;
		return Callback::Finish();
	}


// Register the handler (basing on xmlname) in the Handler Factory
template class HandlerFactory::Register< GenericAsk< cbKeep > >;

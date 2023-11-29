#include <stdio.h>
#include <assert.h>
#include <mpi.h>
#include "cross.h"
#include "vtkLattice.h"
#include "Global.h"

int vtkWriteLattice(const std::string& filename, CartLattice& lattice, const UnitEnv& units, const name_set& what, const lbRegion& total_output_reg)
{
	const lbRegion& local_reg = lattice.getLocalRegion();
	const lbRegion reg = local_reg.intersect(total_output_reg);
        size_t size = reg.size();
	myprint(1,-1,"Writing region %dx%dx%d + %d,%d,%d (size %d) from %dx%dx%d + %d,%d,%d",
		reg.nx,reg.ny,reg.nz,reg.dx,reg.dy,reg.dz, size,
		local_reg.nx,local_reg.ny,local_reg.nz,local_reg.dx,local_reg.dy,local_reg.dz);

	vtkFileOut vtkFile(MPMD.local);
	if (vtkFile.Open(filename.c_str())) return -1;
	double spacing = 1/units.alt("m");
	vtkFile.Init(total_output_reg, reg, "Scalars=\"rho\" Vectors=\"velocity\"", spacing, lattice.px*spacing, lattice.py*spacing, lattice.pz*spacing);

	{
                auto NodeType = std::make_unique<flag_t[]>(size);
		lattice.GetFlags(reg, NodeType.get());
		if (what.explicitlyIn("flag")) {
			vtkFile.WriteField("flag",NodeType.get());
		}
		auto small = std::make_unique<unsigned char[]>(size);
		for (const Model::NodeTypeGroupFlag& it : lattice.model->nodetypegroupflags) {
			if ((what.all && it.isSave) || what.explicitlyIn(it.name)) {
				for (size_t i=0;i<size;i++) {
					small[i] = (NodeType[i] & it.flag) >> it.shift;
				}
				vtkFile.WriteField(it.name.c_str(), small.get());
			}
		}
	}

	for (const Model::Quantity& it : lattice.model->quantities) {
		if (what.in(it.name)) {
			double v = units.alt(it.unit);
			int comp = 1;
			if (it.isVector) comp = 3;
                        auto tmp = std::make_unique<real_t[]>(size*comp);
                        lattice.GetQuantity(it.id, reg, tmp.get(), 1/v);
			vtkFile.WriteField(it.name.c_str(), tmp.get(), comp);
		}
	}
	vtkFile.Finish();
	vtkFile.Close();
	return 0;
}

int binWriteLattice(const std::string& filename, CartLattice& lattice, const UnitEnv& units)
{
	const lbRegion& reg = lattice.getLocalRegion();
	FILE * f;
        int size = reg.size();
	for (const Model::Quantity& it : lattice.model->quantities) {
		int comp = 1;
		if (it.isVector) comp = 3;
                auto tmp = std::make_unique<real_t[]>(size*comp);
		lattice.GetQuantity(it.id, reg, tmp.get(), 1);
		const auto fn = formatAsString("%s.%s.bin", filename, it.name);
		f = fopen(fn.c_str(), "w");
		if (f == NULL) {
			ERROR("Cannot open file: %s\n", fn.c_str());
			return -1;
		}
		fwrite(tmp.get(), sizeof(real_t)*comp, size, f);
		fclose(f);
	}
	return 0;
}



inline int txtWriteElement(FILE * f, float tmp) { return fprintf(f, "%.8g" , tmp); }
inline int txtWriteElement(FILE * f, double tmp) { return fprintf(f, "%.16lg" , tmp); }
inline int txtWriteElement(FILE * f, vector_t tmp) {
	txtWriteElement(f, tmp.x);
	fprintf(f," ");
	txtWriteElement(f, tmp.y);
	fprintf(f," ");
	return txtWriteElement(f, tmp.z);
}

template <typename T> int txtWriteField(FILE * f, T * tmp, int stop, int n)
{
	for (int i=0;i<n;i++) {
		txtWriteElement(f, tmp[i]);
		if (((i+1) % stop) == 0) fprintf(f,"\n"); else fprintf(f, " ");
	}
	return 0;
}


int txtWriteLattice(const std::string& filename, CartLattice& lattice, const UnitEnv& units, const name_set& what, int type)
{
	const lbRegion& reg = lattice.getLocalRegion();
	int size = reg.size();
	if (D_MPI_RANK == 0) {
		const auto fn = formatAsString("%s_info.txt",filename);
		FILE * f = fopen(fn.c_str(),"w");
		if (f == NULL) {
			ERROR("Cannot open file: %s\n", fn.c_str());
			return -1;
		}
		fprintf(f,"dx: %lg\n", 1/units.alt("m"));
		fprintf(f,"dt: %lg\n", 1/units.alt("s"));
		fprintf(f,"dm: %lg\n", 1/units.alt("kg"));
		fprintf(f,"dT: %lg\n", 1/units.alt("K"));
		fprintf(f,"size: %d\n", size);
		fprintf(f,"NX: %d\n", reg.nx);
		fprintf(f,"NY: %d\n", reg.ny);
		fprintf(f,"NZ: %d\n", reg.nz);
		fclose(f);
	}

	for (const Model::Quantity& it : lattice.model->quantities) {
		if (what.in(it.name)) {
			const auto fn = formatAsString("%s_%s.txt", filename, it.name);
			FILE * f=NULL;
			switch (type) {
			case 0:
				f = fopen(fn.c_str(), "w");
				break;
			case 1: {
                                const auto com = formatAsString("gzip > %s.gz", fn);
                                f = popen(com.c_str(), "w");
                                break;
                        }
			default:
				ERROR("Unknown type in txtWriteLattice\n");
			}
			if (f == NULL) {
				ERROR("Cannot open file: %s\n", fn.c_str());
				return -1;
			}
			double v = units.alt(it.unit);
                        auto tmp = std::make_unique<real_t[]>(size);
			lattice.GetQuantity(it.id, reg, tmp.get(), 1/v);
			txtWriteField(f, tmp.get(), reg.nx, size);
			fclose(f);
		}
	}

	return 0;
}

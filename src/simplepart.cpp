#include "Global.h"
#include "MPMD.hpp"
#include "RemoteForceInterface.hpp"
#include "pugixml.hpp"
#include <math.h>
#include <vector>

const double twopi = 8*atan(1.0);
const double pi = 4*atan(1.0);

struct Particle {
  double x[3];
  double r;
  double m;
  double v[3];
  double v0[3];
  double f[3];
  double favg[3];
  double omega[3];
  double omega0[3];
  double torque[3];
  double ease_in_time;
  size_t n;
  bool logging;
  Particle() {
    n = 0;
    for (int i=0; i<3; i++) {
      x[i] = 0;
      v[i] = 0;
      f[i] = 0;
      favg[i] = 0;
      omega[i] = 0;
      torque[i] = 0;
    }
    m = 0;
    r = 0;
    logging = false;
    ease_in_time = 0;
  }
};

struct attr_name_t {
  std::string vector;
  std::string non_vector;
  int d;
  attr_name_t(const std::string& name) {
    bool vec = true;
    d = -1;
    auto w = name.back();
    if (w == 'x') { d = 0; }
    else if (w == 'y') { d = 1; }
    else if (w == 'z') { d = 2; }
    else { vec = false; }
    if (vec) {
      vector = name;
      vector.pop_back();
      non_vector = "=";
    } else {
      non_vector = name;
      vector = "=";
    }
  }
  attr_name_t(const char* name) : attr_name_t(std::string(name)) {};
  bool operator==(const std::string& name) {
    return non_vector == name;
  }
};

typedef std::vector<Particle> Particles;

int main(int argc, char *argv[]) {
  int ret;
  MPMDHelper MPMD;
  MPI_Init(&argc, &argv);
  MPMD.Init(MPI_COMM_WORLD, "SIMPLEPART");
  DEBUG_SETRANK(MPMD.local_rank);
  InitPrint(DEBUG_LEVEL, 6, 8);
  if (MPMD.local_size > 1) {
    ERROR("simplepart: Can only run on single MPI rank");
    return -1;
  }
  MPMD.Identify();
  rfi::RemoteForceInterface<rfi::ForceIntegrator, rfi::RotParticle, rfi::ArrayOfStructures, real_t> RFI;
  RFI.name = "SIMPLEPART";

  MPMDIntercomm inter = MPMD["TCLB"];
  ret = RFI.Connect(MPMD.work, inter.work);
  if (ret)
    return ret;
  assert(RFI.Connected());

  std::vector<size_t> wsize, windex;
  wsize.resize(RFI.Workers());
  windex.resize(RFI.Workers());
  Particles particles;
  double dt = RFI.auto_timestep;

  bool logging = false;
  std::string logging_filename = "";
  if (RFI.hasVar("output")) logging_filename = RFI.getVar("output") + "_SP_Log.csv";
  int logging_iter = 1;
  FILE* logging_f = NULL;
  bool avg = false;

  bool log_position = true;
  bool log_velocity = true;
  bool log_force = true;
  bool log_omega = false;
  bool log_torque = false;

  double periodicity[3], periodic_origin[3];
  bool periodic[3];
  for (int i = 0; i < 3; i++) {
    periodic[i] = false;
    periodicity[i] = 0.0;
    periodic_origin[i] = 0.0;
  }

  double acc_vec[3];
  double acc_freq;
  for (int i = 0; i < 3; i++) acc_vec[i] = 0.0;
  acc_freq = 0.0;

  if (argc < 1 || argc > 2) {
    printf("Syntax: simplepart config.xml\n");
    printf("  You can omit config.xml if configuration is provided by the force calculator (eg. TCLB xml)\n");
    MPI_Abort(MPI_COMM_WORLD,1);
    exit(1);
  }

  char * filename = NULL;
  if (argc > 1) {
    filename = argv[1];
  }
  pugi::xml_document config;
  pugi::xml_parse_result result;
  if (filename != NULL) {
    if (RFI.hasVar("content")) {
      WARNING("Ignoring content (configuration) sent by calculator");
    }
    result = config.load_file(filename, pugi::parse_default | pugi::parse_comments);
  } else {
    if (RFI.hasVar("content")) {
      result = config.load_string(RFI.getVar("content").c_str(), pugi::parse_default | pugi::parse_comments);
    } else {
      printf("No configuration provided (either xml file or content from force calculator\n");
      MPI_Abort(MPI_COMM_WORLD,1);
      exit(1);
    }
  }
  if (!result) {
    ERROR("Error while parsing %s: %s\n", filename, result.description());
    return -1;
  }

  pugi::xml_node main_node = config.child("SimplePart");
  if (! main_node) {
    ERROR("No SimplePart element in %s", filename);
    return -1;
  }
  for (pugi::xml_attribute attr = main_node.first_attribute(); attr; attr = attr.next_attribute()) {
    std::string attr_name = attr.name();
    if (attr_name == "dt") {
      dt = attr.as_double();
    } else if (attr_name == "ax") {
      acc_vec[0] = attr.as_double();
    } else if (attr_name == "ay") {
      acc_vec[1] = attr.as_double();
    } else if (attr_name == "az") {
      acc_vec[2] = attr.as_double();
    } else if (attr_name == "afreq") {
      acc_freq = attr.as_double();
    } else {
      ERROR("Unknown atribute '%s' in '%s'", attr.name(), main_node.name());
      return -1;
    }
  }

  for (pugi::xml_node node = main_node.first_child(); node; node = node.next_sibling()) {
    std::string node_name = node.name();
    if (node_name == "Particle") {
      Particle p;
      for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        attr_name_t attr_name = attr.name();
        if (attr_name.vector == "") {
          p.x[attr_name.d] = attr.as_double();
        } else if (attr_name.vector == "v") {
          p.v0[attr_name.d] = attr.as_double();
        } else if (attr_name.vector == "omega") {
          p.omega0[attr_name.d] = attr.as_double();
        } else if (attr_name == "r") {
          p.r = attr.as_double();
        } else if (attr_name == "m") {
          p.m = attr.as_double();
        } else if (attr_name == "log") {
          p.logging = attr.as_bool();
        } else if (attr_name == "ease-in") {
          p.ease_in_time = attr.as_double();
        } else {
          ERROR("Unknown atribute '%s' in '%s'", attr.name(), node.name());
          return -1;
        }
      }
      if (p.r <= 0.0) {
        ERROR("Specify the radius with 'r' attribute");
        return -1;
      }
      if (p.ease_in_time > 0) {
        for (int i=0;i<3;i++) {
          p.v[i] = 0;
          p.omega[i] = 0;
        }
      } else {
        for (int i=0;i<3;i++) {
          p.v[i] = p.v0[i];
          p.omega[i] = p.omega0[i];
        }
      }
      p.n = particles.size();
      particles.push_back(p);
    } else if (node_name == "Periodic") {
      for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        attr_name_t attr_name = attr.name();
        if (attr_name.vector == "") {
          periodic[attr_name.d] = true;
          periodicity[attr_name.d] = attr.as_double();
        } else if (attr_name.vector == "p") {
          periodic_origin[attr_name.d] = attr.as_double();
        } else {
          ERROR("Unknown atribute '%s' in '%s'", attr.name(), node.name());
          return -1;
        }
      }
    } else if (node_name == "Log") {
      if (logging) {
          ERROR("There can be only one '%s' element", node.name());
          return -1;
      } 
      for (pugi::xml_attribute attr = node.first_attribute(); attr; attr = attr.next_attribute()) {
        std::string attr_name = attr.name();
        logging = true;
        if (attr_name == "name") {
          logging_filename = attr.value();
        } else if (attr_name == "Iterations") {
          logging_iter = attr.as_int();
          if (logging_iter < 1) {
            ERROR("The '%s' attribute in '%s' have to be higher then 1", attr.name(), node.name());
            return -1;
          }
        } else if (attr_name == "average") {
          avg = attr.as_bool();
          if (avg) notice("SIMPLEPART: Particle force averaging is ON");
        } else if (attr_name == "rotation") {
          log_omega = attr.as_bool();
          log_torque = log_omega;
          if (log_omega) notice("SIMPLEPART: Particle omega and torque is logged");
        } else {
          ERROR("Unknown atribute '%s' in '%s'", attr.name(), node.name());
          return -1;
        }
      }
      if (logging && logging_filename == "") {
        ERROR("Loggin file name not set in '%s' element", node.name());
        return -1;
      }
    } else {
      ERROR("Unknown node '%s' in '%s'", node.name(), main_node.name());
      return -1;
    }
  }
  if (logging) {
    logging_f = fopen(logging_filename.c_str(), "w");
    if (logging_f == NULL) {
      ERROR("Failed to open '%s' for writing", logging_filename.c_str());
      return -1;
    }
    fprintf(logging_f, "Iteration,Time");
    for (Particles::iterator p = particles.begin(); p != particles.end(); p++) if (p->logging) {
      size_t n = p->n;
      if (log_position) fprintf(logging_f, ",p%2$ld_%1$sx,p%2$ld_%1$sy,p%2$ld_%1$sz","",n);
      if (log_velocity) fprintf(logging_f, ",p%2$ld_%1$sx,p%2$ld_%1$sy,p%2$ld_%1$sz","v",n);
      if (log_force) fprintf(logging_f, ",p%2$ld_%1$sx,p%2$ld_%1$sy,p%2$ld_%1$sz","f",n);
      if (log_omega) fprintf(logging_f, ",p%2$ld_%1$sx,p%2$ld_%1$sy,p%2$ld_%1$sz","o",n);
      if (log_torque) fprintf(logging_f, ",p%2$ld_%1$sx,p%2$ld_%1$sy,p%2$ld_%1$sz","t",n);
    }
    fprintf(logging_f, "\n");
  }
  for (Particles::iterator p = particles.begin(); p != particles.end(); p++) {
    for (int i=0; i<3; i++) p->v[i] = p->v[i] - acc_vec[i] / 2.0;
  }
  int iter = 0;
  while (RFI.Active()) {
    for (int phase = 0; phase < 3; phase++) {
      if (phase == 0) {
        for (int i = 0; i < RFI.Workers(); i++)
          wsize[i] = 0;
      } else {
        for (int i = 0; i < RFI.Workers(); i++)
          windex[i] = 0;
      }

      for (Particles::iterator p = particles.begin(); p != particles.end(); p++) {
        if (phase == 2) {
          p->f[0] = 0;
          p->f[1] = 0;
          p->f[2] = 0;
          p->torque[0] = 0;
          p->torque[1] = 0;
          p->torque[2] = 0;
        }
        int minper[3], maxper[3], d[3];
        size_t offset = 0;
        for (int worker = 0; worker < RFI.Workers(); worker++) {
          for (int j = 0; j < 3; j++) {
            double prd = periodicity[j];
            double lower = 0;
            double upper = periodicity[j];
            if (RFI.WorkerBox(worker).declared) {
              lower = RFI.WorkerBox(worker).lower[j];
              upper = RFI.WorkerBox(worker).upper[j];
            }
            if (periodic[j]) {
              maxper[j] = floor((upper - p->x[j] + p->r) / prd);
              minper[j] = ceil((lower - p->x[j] - p->r) / prd);
            } else {
              if ((p->x[j] + p->r >= lower) && (p->x[j] - p->r <= upper)) {
                minper[j] = 0;
                maxper[j] = 0;
              } else {
                minper[j] = 0;
                maxper[j] = -1; // no balls
              }
            }
          }

          int copies = (maxper[0] - minper[0] + 1) * (maxper[1] - minper[1] + 1) * (maxper[2] - minper[2] + 1);
          for (d[0] = minper[0]; d[0] <= maxper[0]; d[0]++) {
            for (d[1] = minper[1]; d[1] <= maxper[1]; d[1]++) {
              for (d[2] = minper[2]; d[2] <= maxper[2]; d[2]++) {
                double px[3];
                for (int j = 0; j < 3; j++)
                  px[j] = p->x[j] + d[j] * periodicity[j];
                if (phase == 0) {
                  wsize[worker]++;
                } else {
                  size_t i = offset + windex[worker];
                  if (phase == 1) {
                    RFI.setData(i, RFI_DATA_R, p->r);
                    RFI.setData(i, RFI_DATA_POS + 0, px[0]);
                    RFI.setData(i, RFI_DATA_POS + 1, px[1]);
                    RFI.setData(i, RFI_DATA_POS + 2, px[2]);
                    RFI.setData(i, RFI_DATA_VEL + 0, p->v[0]);
                    RFI.setData(i, RFI_DATA_VEL + 1, p->v[1]);
                    RFI.setData(i, RFI_DATA_VEL + 2, p->v[2]);
                    if (RFI.Rot()) {
                      RFI.setData(i, RFI_DATA_ANGVEL + 0, p->omega[0]);
                      RFI.setData(i, RFI_DATA_ANGVEL + 1, p->omega[1]);
                      RFI.setData(i, RFI_DATA_ANGVEL + 2, p->omega[2]);
                    }
                  } else {
                    p->f[0] += RFI.getData(i, RFI_DATA_FORCE + 0);
                    p->f[1] += RFI.getData(i, RFI_DATA_FORCE + 1);
                    p->f[2] += RFI.getData(i, RFI_DATA_FORCE + 2);
                    if (RFI.Rot()) {
                      p->torque[0] += RFI.getData(i, RFI_DATA_MOMENT + 0);
                      p->torque[1] += RFI.getData(i, RFI_DATA_MOMENT + 1);
                      p->torque[2] += RFI.getData(i, RFI_DATA_MOMENT + 2);
                    }
                  }
                  windex[worker]++;
                }
              }
            }
          }
          offset += wsize[worker];
        }
      }
      if (phase == 0) {
        for (int worker = 0; worker < RFI.Workers(); worker++)
          RFI.Size(worker) = wsize[worker];
        RFI.SendSizes();
        RFI.Alloc();
      } else if (phase == 1) {
        RFI.SendParticles();
        RFI.SendForces();
      } else {
      }
    }
    if (logging && (iter % logging_iter == 0)) {
      fprintf(logging_f, "%d,%.15lg", iter, dt*iter);
      for (Particles::iterator p = particles.begin(); p != particles.end(); p++) if (p->logging) {
        if (log_position) for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->x[i]);
        if (log_velocity) for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->v[i]);
        if (log_force) {
          if (avg) {
            for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->favg[i]/logging_iter);
            for (int i=0; i<3; i++) p->favg[i] = 0;
          } else {
            for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->f[i]);
          }
        }
        if (log_omega) for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->omega[i]);
        if (log_torque) for (int i=0; i<3; i++) fprintf(logging_f, ",%.15lg", p->torque[i]);
      }
      fprintf(logging_f, "\n");
    }
    for (Particles::iterator p = particles.begin(); p != particles.end(); p++) {
      double t = dt * iter;
      for (int i=0; i<3; i++) p->favg[i] = p->favg[i] + p->f[i];
      if (p->ease_in_time > 0) {
        if (p->ease_in_time > t) {
          double fac = (1-cos(pi * t / p->ease_in_time))*0.5;
          for (int i=0; i<3; i++) p->v[i] = p->v0[i] * fac;
          for (int i=0; i<3; i++) p->omega[i] = p->omega0[i] * fac;
        } else {
          for (int i=0; i<3; i++) p->v[i] = p->v0[i];
          for (int i=0; i<3; i++) p->omega[i] = p->omega0[i];
          p->ease_in_time = 0;
        }
      } else {
        if (p->m > 0.0) {
          for (int i=0; i<3; i++) p->v[i] = p->v[i] + p->f[i] / p->m * dt;
        }
        for (int i=0; i<3; i++) p->v[i] = p->v[i] + acc_vec[i] * cos(twopi * t * acc_freq);
      }
      for (int i=0; i<3; i++) p->x[i] = p->x[i] + p->v[i] * dt;
    }
    iter++;
  }
  if (logging && (logging_f != NULL)) fclose(logging_f);
  if (RFI.Connected()) {
    RFI.Close();
  }
  MPI_Finalize();
  return 0;
}

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "../../general/forall.hpp"

using namespace std;
using namespace mfem;

static const double omega = 2.0 * M_PI * 5.3;

double exactFun(Vector& x)
{
   return std::sin(omega * (x[0]+x[1]) / std::sqrt(2));
}

class PoissonMultigridOperator : public TimedMultigridOperator
{
 private:
   Array<ParBilinearForm*> forms;
   bool partialAssembly{true};
   bool ownLORMatrix{false};
   Coefficient* coeff{nullptr};
   HypreParMatrix* hypreCoarseMat{nullptr};
   ParBilinearForm* a_pc{nullptr};
   ParMesh* pmesh_lor{nullptr};
   H1_FECollection* fec_lor{nullptr};
   ParFiniteElementSpace* fespace_lor{nullptr};
   bool useCoarsePCG{false};
   HypreBoomerAMG* amg{nullptr};
   GMRESSolver* coarsePCGSolver{nullptr};

   void AddIntegrators(BilinearForm* form)
   {
      form->AddDomainIntegrator(new DiffusionIntegrator(*coeff));
      ConstantCoefficient* massCoeff = new ConstantCoefficient(-omega * omega);
      form->AddDomainIntegrator(new MassIntegrator(*massCoeff));
   }

   Operator* ConstructOperator(ParFiniteElementSpace* fespace,
                               const Array<int>& essentialDofs)
   {
      ParBilinearForm* form = new ParBilinearForm(fespace);
      if (partialAssembly)
      {
         form->SetAssemblyLevel(AssemblyLevel::PARTIAL);
      }
      AddIntegrators(form);
      if (!partialAssembly)
      {
         form->UsePrecomputedSparsity();
      }
      form->Assemble();

      OperatorPtr opr;

      if (partialAssembly)
      {
         opr.SetType(Operator::ANY_TYPE);
      }
      else
      {
         opr.SetType(Operator::Hypre_ParCSR);
      }

      form->FormSystemMatrix(essentialDofs, opr);
      opr.SetOperatorOwner(false);

      forms.Append(form);

      return opr.Ptr();
   }

   Solver* ConstructCoarseSolver(ParMesh* mesh, Operator* opr,
                                 const Array<int>& essentialDofs,
                                 int coarseOrder, int coarseSteps)
   {
      // Reuse matrix for AMG
      if (!partialAssembly && coarseOrder == 1)
      {
         hypreCoarseMat = dynamic_cast<HypreParMatrix*>(opr);
      }
      else
      {
         if (coarseOrder > 1)
         {
            pmesh_lor = new ParMesh(mesh, coarseOrder, BasisType::GaussLobatto);
            fec_lor = new H1_FECollection(1, mesh->Dimension(),
                                          BasisType::GaussLobatto);
            fespace_lor = new ParFiniteElementSpace(pmesh_lor, fec_lor);
            a_pc = new ParBilinearForm(fespace_lor);
         }
         else
         {
            fec_lor = new H1_FECollection(1, mesh->Dimension(),
                                          BasisType::GaussLobatto);
            fespace_lor = new ParFiniteElementSpace(mesh, fec_lor);
            a_pc = new ParBilinearForm(fespace_lor);
         }

         AddIntegrators(a_pc);
         a_pc->UsePrecomputedSparsity();
         a_pc->Assemble();

         hypreCoarseMat = new HypreParMatrix();
         a_pc->FormSystemMatrix(essentialDofs, *hypreCoarseMat);
         ownLORMatrix = true;
      }

      amg = new HypreBoomerAMG(*hypreCoarseMat);
      amg->SetPrintLevel(-1);
      amg->SetMaxIter(coarseSteps);

      // if (useCoarsePCG)
      // {
         amg->SetMaxIter(1);
         coarsePCGSolver = new GMRESSolver(MPI_COMM_WORLD);
         coarsePCGSolver->SetPrintLevel(0);
         coarsePCGSolver->SetMaxIter(2000);
         coarsePCGSolver->SetRelTol(1e-4);
         coarsePCGSolver->SetAbsTol(0.0);
         coarsePCGSolver->SetOperator(*opr);
         // coarsePCGSolver->SetPreconditioner(*amg);
         return coarsePCGSolver;
      // }
      // else
      // {
      //    return amg;
      // }
   }

 public:
   PoissonMultigridOperator(ParMesh* mesh, ParFiniteElementSpace* fespace,
                            const Array<int>& essentialDofs, int coarseOrder,
                            bool partialAssembly_, int coarseSteps,
                            bool useCoarsePCG_, bool jump)
       : TimedMultigridOperator(), partialAssembly(partialAssembly_),
         useCoarsePCG(useCoarsePCG_)
   {
      if (jump)
      {
         auto f = [](const Vector& x) {
            // return (x[0] < 0.5) ? 1.0 : 100.0;
            return 5 * x[0] + 1.0;
         };
         coeff = new FunctionCoefficient(f);
      }
      else
      {
         coeff = new ConstantCoefficient(1.0);
      }

      Operator* coarseOpr = ConstructOperator(fespace, essentialDofs);
      Solver* coarseSolver = ConstructCoarseSolver(
          mesh, coarseOpr, essentialDofs, coarseOrder, coarseSteps);

      AddCoarsestLevel(coarseOpr, coarseSolver, partialAssembly, true);
   }

   ~PoissonMultigridOperator()
   {
      for (int i = 0; i < forms.Size(); ++i)
      {
         delete forms[i];
      }

      if (useCoarsePCG)
      {
         delete amg;
      }

      if (ownLORMatrix)
      {
         delete hypreCoarseMat;
      }

      delete a_pc;
      delete pmesh_lor;
      delete fespace_lor;
      delete fec_lor;
      delete coeff;
   }

   Solver* ConstructSmoother(ParFiniteElementSpace* fespace,
                             Operator* solveOperator,
                             const Array<int>& essentialDofs,
                             int chebyshevOrder)
   {
      Solver* smoother = nullptr;

      if (partialAssembly)
      {
         Vector* diag = new Vector(fespace->GetTrueVSize());
         forms.Last()->AssembleDiagonal(*diag);

         Vector* coeffDiag = new Vector(fespace->GetTrueVSize());
         {
            Array<int> local_dofs;
            int ne = fespace->GetNE();
            const IntegrationRule& ir = fespace->GetFE(0)->GetNodes();
            int nq = ir.GetNPoints();

            for(int e = 0; e < ne; ++e)
            {
               fespace->GetElementDofs(e, local_dofs);
               ElementTransformation& T = *fespace->GetElementTransformation(e);
               for (int q = 0; q < nq; ++q)
               {
                  (*coeffDiag)[local_dofs[q]] = 1.0 / sqrt(coeff->Eval(T, ir.IntPoint(q)));
               }
            }
         }

         Mesh* pmesh_lor = new Mesh(fespace->GetMesh(), fespace->GetOrder(0), BasisType::GaussLobatto);
         H1_FECollection* fec_lor_ = new H1_FECollection(1, pmesh_lor->Dimension(), BasisType::GaussLobatto);
         FiniteElementSpace* fespace_lor_ = new FiniteElementSpace(pmesh_lor, fec_lor_);

         BilinearForm* a_pc_ = new BilinearForm(fespace_lor_);
         a_pc_->SetAssemblyLevel(AssemblyLevel::FULL);
         AddIntegrators(a_pc_);
         // ConstantCoefficient* constCoeff = new ConstantCoefficient(1.0);
         // a_pc_->AddDomainIntegrator(new DiffusionIntegrator(*constCoeff));
         a_pc_->UsePrecomputedSparsity();
         a_pc_->Assemble();
         
         // BilinearForm* a_pc_pa = new BilinearForm(fespace_lor_);
         // a_pc_pa->SetAssemblyLevel(AssemblyLevel::PARTIAL);
         // AddIntegrators(a_pc_pa);
         // a_pc_pa->Assemble();
         Vector* LORdiag = new Vector(fespace->GetTrueVSize());
         // a_pc_pa->AssembleDiagonal(*LORdiag);

         SparseMatrix* LORmat = new SparseMatrix();
         a_pc_->FormSystemMatrix(essentialDofs, *LORmat);

         PowerMethod powerMethod(MPI_COMM_WORLD);

         // AdditiveSchwarzApproxLORSmoother test(*fespace, essentialDofs, *forms.Last(), *coeffDiag, LORmat, 1.0);
         AdditiveSchwarzApproxLORSmoother test(*fespace, essentialDofs, *forms.Last(), *coeffDiag, *LORdiag, LORmat, 1.0);
         // OperatorJacobiSmoother test(*diag, essentialDofs, 1.0);


         ProductOperator powerOperator(&test, solveOperator, false, false);
         Vector ev(solveOperator->Width());
         double estLargestEigenvalue = powerMethod.EstimateLargestEigenvalue(powerOperator, ev, 10, 1e-8);

         std::cout << "ev = " << estLargestEigenvalue << std::endl;

         double upper_bound = 1.1 * estLargestEigenvalue;
         double lower_bound = 0.0 * estLargestEigenvalue;
         double theta = 0.5 * (upper_bound + lower_bound);
         double delta = 0.5 * (upper_bound - lower_bound);
         double weight = 1.0 / theta;
         std::cout << "weight = " << weight << std::endl;

         // HypreParMatrix* LORmatp = new HypreParMatrix();
         // a_pc_->FormSystemMatrix(essentialDofs, *LORmatp);

         // SparseMatrix* LORmat = new SparseMatrix();
         // LORmatp->GetDiag(*LORmat);

         std::cout << "truevsize = " << fespace->GetTrueVSize() << std::endl;
         std::cout << "Width = " << LORmat->Width() << std::endl;

         smoother = new AdditiveSchwarzApproxLORSmoother(*fespace, essentialDofs, *forms.Last(), *coeffDiag, *LORdiag, LORmat, weight);
         // smoother = new ElementWiseJacobi(*fespace, *forms.Last(), *diag, essentialDofs, 2.0/3.0);
         // smoother = new OperatorJacobiSmoother(*diag, essentialDofs, weight);
         // smoother = new OperatorChebyshevSmoother(solveOperator, *diag, essentialDofs, chebyshevOrder, estLargestEigenvalue);
         

         // Vector ev(solveOperator->Width());
         // OperatorJacobiSmoother invDiagOperator(*diag, essentialDofs, 1.0);
         // ProductOperator diagPrecond(&invDiagOperator, solveOperator, false,
         //                             false);

         // PowerMethod powerMethod(MPI_COMM_WORLD);
         // double estLargestEigenvalue =
         //     powerMethod.EstimateLargestEigenvalue(diagPrecond, ev, 10, 1e-8);
         // smoother = new OperatorChebyshevSmoother(solveOperator, *diag,
         //                                          essentialDofs, chebyshevOrder,
         //                                          estLargestEigenvalue);
      }
      else
      {
         smoother =
             new HypreSmoother(static_cast<HypreParMatrix&>(*solveOperator));
      }

      return smoother;
   }

   void AddLevel(ParFiniteElementSpace* lFEspace,
                 ParFiniteElementSpace* hFEspace,
                 const Array<int>& essentialDofs, int chebyshevOrder)
   {
      Operator* opr = ConstructOperator(hFEspace, essentialDofs);
      Solver* smoother =
          ConstructSmoother(hFEspace, opr, essentialDofs, chebyshevOrder);
      Operator* P = new TrueTransferOperator(*lFEspace, *hFEspace);
      MultigridOperator::AddLevel(opr, smoother, P, partialAssembly, true,
                                  true);
   }

   void FormLinearSystem(const Array<int>& ess_tdof_list, Vector& x, Vector& b,
                         Vector& X, Vector& B, int copy_interior = 0)
   {
      OperatorPtr dummy;
      forms.Last()->FormLinearSystem(ess_tdof_list, x, b, dummy, X, B,
                                     copy_interior);
   }

   void RecoverFEMSolution(const Vector& X, const Vector& b, Vector& x)
   {
      forms.Last()->RecoverFEMSolution(X, b, x);
   }
};

int main(int argc, char* argv[])
{
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 1. Parse command-line options.
   const char* mesh_file = "../../data/inline-quad.mesh";
   int ref_levels = 0;
   int pref_levels = 0;
   int order = 1;
   int h_levels = 2;
   int o_levels = 1;
   int smoothingSteps = 3;
   int coarseSteps = 2;
   int chebyshevOrder = 3;
   bool visualization = 1;
   bool partialAssembly = true;
   const char* precondInput = "MG";
   bool useCoarsePCG = false;
   bool jump = false;

   enum class Method
   {
      MG = 0,
      LOR = 1,
      LORS = 2
   } method;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh", "Mesh file to use.");
   args.AddOption(
       &ref_levels, "-r", "--refine",
       "Number of times to refine the initial mesh uniformly;"
       "This mesh will be the coarse mesh in the multigrid hierarchy");
   args.AddOption(
       &pref_levels, "-pr", "--parallelrefine",
       "Number of times to refine the serially refined mesh in parallel;"
       "This mesh will be the coarse mesh in the multigrid hierarchy");
   args.AddOption(&order, "-o", "--order",
                  "Order of the finite element spaces");
   args.AddOption(&h_levels, "-hl", "--hlevels",
                  "Number of geometric levels in the multigrid hierarchy");
   args.AddOption(&o_levels, "-ol", "--orderlevels",
                  "Number of order levels in the multigrid hierarchy");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&partialAssembly, "-pa", "--partialassembly", "-no-pa",
                  "--no-partialassembly",
                  "Enable or disable partial assembly.");
   args.AddOption(&precondInput, "-p", "--precond",
                  "Preconditioner: MG - Multigrid, LOR = Low-order refined, "
                  "LORS = Low-order refined with smoothing");
   args.AddOption(&smoothingSteps, "-ss", "--smoothingsteps",
                  "Number of pre- and post-smoothing steps");
   args.AddOption(&coarseSteps, "-cs", "--coarsesteps",
                  "Number of coarse grid corrections");
   args.AddOption(
       &chebyshevOrder, "-co", "--chebyshevorder",
       "Chebyshev smoother order. Order 1 corresponds to damped Jacobi");
   args.AddOption(&useCoarsePCG, "-cpcg", "--coarsepcg", "-no-cpcg",
                  "--no-coarsepcg", "Enable or disable PCG as a coarse solver");
   args.AddOption(&jump, "-jump", "--jump", "-no-jump", "--no-jump",
                  "Enable or disable coefficient jump");

   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }

   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   if (o_levels > 1 && order > 1)
   {
      MFEM_ABORT("Order refinements are not supported with order > 1");
   }

   std::map<std::string, Method> mapInputToPrecond = {
       {"MG", Method::MG},
       {"LOR", Method::LOR},
       {"LORS", Method::LORS},
   };

   auto it = mapInputToPrecond.find(std::string(precondInput));
   if (it == mapInputToPrecond.end())
   {
      MFEM_ABORT("Method " << precondInput << " not found");
   }
   method = it->second;

   // See class BasisType in fem/fe_coll.hpp for available basis types
   int basis = BasisType::GaussLobatto;
   if (myid == 0)
   {
      cout << "Using " << BasisType::Name(basis) << " basis ..." << endl;
   }

   // 2. Read the mesh from the given mesh file. We can handle triangular,
   //    quadrilateral, tetrahedral, hexahedral, surface and volume meshes with
   //    the same code.
   // Mesh* mesh = new Mesh(mesh_file, 1, 1);
   Mesh* mesh = new Mesh(1,1, Element::QUADRILATERAL, true, 1.0, 1.0, false);
   int dim = mesh->Dimension();

   Array<int> ess_bdr(mesh->bdr_attributes.Max());
   ess_bdr = 1;

   // Initial refinements of the input grid
   for (int i = 0; i < ref_levels; ++i)
   {
      mesh->UniformRefinement();
   }

   ParMesh* pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   mesh = nullptr;

   // Parallel refinements of the serially refined mesh
   for (int i = 0; i < pref_levels; ++i)
   {
      pmesh->UniformRefinement();
   }

   Array<int> orders;
   Array<FiniteElementCollection*> feCollectons;
   orders.Append(order);
   feCollectons.Append(new H1_FECollection(order, dim, basis));

   // Set up coarse grid finite element space
   ParFiniteElementSpace* fespace =
       new ParFiniteElementSpace(pmesh, feCollectons.Last());
   HYPRE_Int size = fespace->GlobalTrueVSize();

   if (myid == 0)
   {
      cout << "Number of finite element unknowns on level 0: " << size << endl;
   }

   Array<Array<int>*> essentialTrueDoFs;
   essentialTrueDoFs.Append(new Array<int>());
   fespace->GetEssentialTrueDofs(ess_bdr, *essentialTrueDoFs.Last());

   // Build hierarchy of meshes and spaces
   // Geometric refinements
   ParSpaceHierarchy* spaceHierarchy =
       new ParSpaceHierarchy(pmesh, fespace, true, true);
   for (int level = 1; level < h_levels; ++level)
   {
      spaceHierarchy->AddUniformlyRefinedLevel();
      orders.Append(order);
      if (myid == 0)
      {
         cout << "h refinement" << endl;
      }
   }

   // Order refinements
   for (int level = 1; level < o_levels; ++level)
   {
      int newOrder = std::pow(2, level);
      feCollectons.Append(new H1_FECollection(newOrder, dim, basis));
      spaceHierarchy->AddOrderRefinedLevel(feCollectons.Last());
      orders.Append(newOrder);
      if (myid == 0)
      {
         cout << "p refinement from order " << std::pow(2, level - 1) << " to "
              << newOrder << endl;
      }
   }

   // Collect essential dofs
   for (int level = 1; level < spaceHierarchy->GetNumLevels(); ++level)
   {
      essentialTrueDoFs.Append(new Array<int>());
      spaceHierarchy->GetFESpaceAtLevel(level).GetEssentialTrueDofs(
          ess_bdr, *essentialTrueDoFs[level]);

      size = spaceHierarchy->GetFESpaceAtLevel(level).GlobalTrueVSize();
      if (myid == 0)
      {
         cout << "Number of finite element unknowns on level " << level << ": "
              << size << endl;
      }
   }

   if (myid == 0)
   {
      cout << "nproc: " << num_procs << endl;
      cout << "Dofs: " << spaceHierarchy->GetFinestFESpace().GlobalTrueVSize()
           << endl;
      cout << "Average dofs per processor: "
           << spaceHierarchy->GetFinestFESpace().GlobalTrueVSize() / num_procs
           << endl;
      cout << "Order: " << orders.Last() << endl;
      cout << "MG levels: " << spaceHierarchy->GetNumLevels() << endl;
   }

   PoissonMultigridOperator* solveOperator = nullptr;

   if (myid == 0)
   {
      cout << "Setting up operators..." << flush;
   }

   tic_toc.Clear();
   tic_toc.Start();

   // Construct multigrid operator depending on method
   if (method == Method::LOR || method == Method::LORS)
   {
      solveOperator = new PoissonMultigridOperator(
          spaceHierarchy->GetFinestFESpace().GetParMesh(),
          &spaceHierarchy->GetFinestFESpace(), *essentialTrueDoFs.Last(),
          orders.Last(), partialAssembly, coarseSteps, useCoarsePCG, jump);

      if (method == Method::LORS)
      {
         Operator* opr = solveOperator->GetOperatorAtLevel(0);
         Operator* identityProlongation =
             new IdentityOperator(solveOperator->Height());
         Solver* smoother = solveOperator->ConstructSmoother(
             &spaceHierarchy->GetFinestFESpace(), opr,
             *essentialTrueDoFs.Last(), chebyshevOrder);

         solveOperator->MultigridOperator::AddLevel(
             opr, smoother, identityProlongation, false, true, true);
      }
   }
   else
   {
      solveOperator = new PoissonMultigridOperator(
          spaceHierarchy->GetFESpaceAtLevel(0).GetParMesh(),
          &spaceHierarchy->GetFESpaceAtLevel(0), *essentialTrueDoFs[0],
          orders[0], partialAssembly, coarseSteps, useCoarsePCG, jump);

      for (int level = 1; level < spaceHierarchy->GetNumLevels(); ++level)
      {
         solveOperator->AddLevel(&spaceHierarchy->GetFESpaceAtLevel(level - 1),
                                 &spaceHierarchy->GetFESpaceAtLevel(level),
                                 *essentialTrueDoFs[level], chebyshevOrder);
      }
   }

   MultigridSolver* preconditioner =
       new MultigridSolver(solveOperator, MultigridSolver::CycleType::VCYCLE,
                           smoothingSteps, smoothingSteps);

   tic_toc.Stop();
   double setupTime = tic_toc.RealTime();
   if (myid == 0)
   {
      cout << " done. Setup time: " << setupTime << "s" << endl;
   }

   ParGridFunction x(&spaceHierarchy->GetFinestFESpace());
   x = 0.0;

   FunctionCoefficient exact(exactFun);
   x.ProjectCoefficient(exact);

   if (myid == 0)
   {
      cout << "Assembling rhs..." << flush;
   }
   tic_toc.Clear();
   tic_toc.Start();
   ParLinearForm* b = new ParLinearForm(&spaceHierarchy->GetFinestFESpace());
   ConstantCoefficient one(1.0);
   // b->AddDomainIntegrator(new DomainLFIntegrator(one));
   b->Assemble();
   tic_toc.Stop();
   if (myid == 0)
   {
      cout << " done, " << tic_toc.RealTime() << "s" << endl;
   }

   Vector X, B;
   solveOperator->FormLinearSystem(*essentialTrueDoFs.Last(), x, *b, X, B);

   tic_toc.Clear();
   tic_toc.Start();

   GMRESSolver pcg(MPI_COMM_WORLD);
   pcg.SetPrintLevel(1);
   pcg.SetMaxIter(1000);
   pcg.SetRelTol(0.0);
   pcg.SetAbsTol(1e-8);
   pcg.SetOperator(*solveOperator);
   pcg.SetPreconditioner(*preconditioner);
   // pcg.SetPreconditioner(*solveOperator->GetSmootherAtLevel(spaceHierarchy->GetFinestLevelIndex()));
   pcg.Mult(B, X);

   tic_toc.Stop();
   double solveTime = tic_toc.RealTime();

   if (myid == 0)
   {
      cout << "Time to solution: " << solveTime << "s" << endl;
      cout << "Total time: " << setupTime + solveTime << "s" << endl;
      if (TimedMultigridOperator* tmg =
              dynamic_cast<TimedMultigridOperator*>(solveOperator))
      {
         // tmg->PrintStats(TimedMultigridOperator::Operation::OPERATOR, cout);
         // tmg->PrintStats(TimedMultigridOperator::Operation::PROLONGATION, cout);
         // tmg->PrintStats(TimedMultigridOperator::Operation::RESTRICTION, cout);
         // tmg->PrintStats(TimedMultigridOperator::Operation::SMOOTHER, cout);
      }
   }

   solveOperator->RecoverFEMSolution(X, *b, x);

   if (visualization)
   {
      char vishost[] = "localhost";
      int visport = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock << "parallel " << num_procs << " " << myid << "\n";
      sol_sock.precision(8);
      sol_sock << "solution\n"
               << *spaceHierarchy->GetFinestFESpace().GetParMesh() << x
               << flush;
   }

   delete preconditioner;
   delete solveOperator;
   delete b;
   delete spaceHierarchy;

   for (int i = 0; i < essentialTrueDoFs.Size(); ++i)
   {
      delete essentialTrueDoFs[i];
   }

   for (int i = 0; i < feCollectons.Size(); ++i)
   {
      delete feCollectons[i];
   }

   MPI_Finalize();

   return 0;
}
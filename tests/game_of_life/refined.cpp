/*
Tests the grid with a game of life on a refined grid in 3 D with neighbors only in the ? plane

Copyright 2010, 2011, 2012, 2013, 2014,
2015, 2016, 2018 Finnish Meteorological Institute

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License version 3
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "algorithm"
#include "cstdlib"
#include "fstream"
#include "iostream"
#include "sstream"
#include "unordered_set"

#include "mpi.h"
#include "zoltan.h"

#include "dccrg_stretched_cartesian_geometry.hpp"
#include "dccrg.hpp"


struct game_of_life_cell {
	unsigned int is_alive, live_neighbor_count;

	std::tuple<void*, int, MPI_Datatype> get_mpi_datatype()
	{
		return std::make_tuple(&(this->is_alive), 1, MPI_UNSIGNED);
	}
};


using namespace std;
using namespace dccrg;

int main(int argc, char* argv[])
{
	if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
		cerr << "Coudln't initialize MPI." << endl;
		abort();
	}

	MPI_Comm comm = MPI_COMM_WORLD;

	int rank = 0, comm_size = 0;
	MPI_Comm_rank(comm, &rank);
	MPI_Comm_size(comm, &comm_size);

	float zoltan_version;
	if (Zoltan_Initialize(argc, argv, &zoltan_version) != ZOLTAN_OK) {
	    cerr << "Zoltan_Initialize failed" << endl;
	    exit(EXIT_FAILURE);
	}

	Dccrg<game_of_life_cell, Stretched_Cartesian_Geometry> grid;

	const std::array<uint64_t, 3> grid_length = {{5, 5, 3}};
	const double cell_length = 1.0 / grid_length[0];

	#define NEIGHBORHOOD_SIZE 1
	grid
		.set_initial_length(grid_length)
		.set_neighborhood_length(NEIGHBORHOOD_SIZE)
		.set_maximum_refinement_level(-1)
		.set_load_balancing_method("RANDOM")
		.initialize(comm)
		.balance_load();

	Stretched_Cartesian_Geometry::Parameters geom_params;
	for (size_t dimension = 0; dimension < grid_length.size(); dimension++) {
		for (size_t i = 0; i <= grid_length[dimension]; i++) {
			geom_params.coordinates[dimension].push_back(double(i) * cell_length);
		}
	}
	grid.set_geometry(geom_params);

	// refine the grid increasingly in the z direction a few times
	for (const auto& cell: grid.local_cells) {
		if (grid.geometry.get_center(cell.id)[2] > 1 * 1.5 * cell_length) {
			grid.refine_completely(cell.id);
		}
	}
	grid.stop_refining();
	grid.balance_load();
	for (const auto& cell: grid.local_cells) {
		if (grid.geometry.get_center(cell.id)[2] > 2 * 1.5 * cell_length) {
			grid.refine_completely(cell.id);
		}
	}
	grid.stop_refining();
	grid.balance_load();

	// initialize the game with a line of living cells in the x direction in the middle
	for (auto& cell: grid.local_cells) {
		cell.data->live_neighbor_count = 0;

		const std::array<double, 3>
			cell_center = grid.geometry.get_center(cell.id),
			cell_length = grid.geometry.get_length(cell.id);

		if (fabs(0.5 + 0.1 * cell_length[1] - cell_center[1]) < 0.5 * cell_length[1]) {
			cell.data->is_alive = 1;
		} else {
			cell.data->is_alive = 0;
		}
	}

	// every process outputs the game state into its own file
	ostringstream basename, suffix(".vtk");
	basename << "tests/game_of_life/refined_" << rank << "_";
	ofstream outfile, visit_file;

	// visualize the game with visit -o game_of_life_test.visit
	if (rank == 0) {
		visit_file.open("tests/game_of_life/refined.visit");
		visit_file << "!NBLOCKS " << comm_size << endl;
	}

	#define TIME_STEPS 25
	for (int step = 0; step < TIME_STEPS; step++) {

		grid.balance_load();
		grid.update_copies_of_remote_neighbors();
		std::vector<uint64_t> cells;
		for (const auto& cell: grid.local_cells) {
			cells.push_back(cell.id);
		}
		// the library writes the grid into a file in ascending cell order,
		// do the same for grid data at every time step
		sort(cells.begin(), cells.end());

		// write the game state into a file named according to the current time step
		string current_output_name("");
		ostringstream step_string;
		step_string.fill('0');
		step_string.width(5);
		step_string << step;
		current_output_name += basename.str();
		current_output_name += step_string.str();
		current_output_name += suffix.str();

		// visualize the game with visit -o game_of_life_test.visit
		if (rank == 0) {
			for (int process = 0; process < comm_size; process++) {
				visit_file << "refined_" << process << "_" << step_string.str() << suffix.str() << endl;
			}
		}


		// write the grid into a file
		grid.write_vtk_file(current_output_name.c_str());
		// prepare to write the game data into the same file
		outfile.open(current_output_name.c_str(), ofstream::app);
		outfile << "CELL_DATA " << cells.size() << endl;

		// go through the grids cells and write their state into the file
		outfile << "SCALARS is_alive float 1" << endl;
		outfile << "LOOKUP_TABLE default" << endl;
		for (const auto& cell: grid.local_cells) {
			if (cell.data->is_alive > 0) {
				outfile << "1";
			} else {
				outfile << "0";
			}
			outfile << endl;

		}

		// write each cells live neighbor count
		outfile << "SCALARS live_neighbor_count float 1" << endl;
		outfile << "LOOKUP_TABLE default" << endl;
		for (const auto& cell: grid.local_cells) {
			outfile << cell.data->live_neighbor_count << endl;
		}

		// write each cells neighbor count
		outfile << "SCALARS neighbors int 1" << endl;
		outfile << "LOOKUP_TABLE default" << endl;
		for (const auto& cell: grid.local_cells) {
			const auto* const neighbors = grid.get_neighbors_of(cell.id);
			outfile << neighbors->size() << endl;
		}

		// write each cells process
		outfile << "SCALARS process int 1" << endl;
		outfile << "LOOKUP_TABLE default" << endl;
		for (size_t i = 0; i < cells.size(); i++) {
			outfile << rank << endl;
		}

		// write each cells id
		outfile << "SCALARS id int 1" << endl;
		outfile << "LOOKUP_TABLE default" << endl;
		for (auto& cell: grid.local_cells) {
			outfile << cell.id << endl;
		}
		outfile.close();

		// get the neighbor counts of every cell
		// FIXME: use the (at some point common) solver from (un)refined2d and only include x and y directions in neighborhood
		for (const auto& cell: grid.local_cells) {
			cell.data->live_neighbor_count = 0;

			for (const auto& neighbor: cell.neighbors_of) {
				// only consider neighbors in the same z plane
				if (
					grid.geometry.get_center(cell.id)[2]
					!= grid.geometry.get_center(neighbor.id)[2]
				) {
					continue;
				}

				if (neighbor.data->is_alive) {
					cell.data->live_neighbor_count++;
				}
			}
		}

		// calculate the next turn
		for (const auto& cell: grid.local_cells) {
			if (cell.data->live_neighbor_count == 3) {
				cell.data->is_alive = 1;
			} else if (cell.data->live_neighbor_count != 2) {
				cell.data->is_alive = 0;
			}
		}

	}

	if (rank == 0) {
		visit_file.close();
	}

	MPI_Finalize();

	return EXIT_SUCCESS;
}
